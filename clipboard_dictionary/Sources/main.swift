import AppKit
import Foundation

struct ManifestEntry: Decodable {
    let word: String
    let file: String
}

struct LookupResponse: Decodable {
    let status: String
    let matches: [Match]
    let fetchErrors: [FetchError]?

    enum CodingKeys: String, CodingKey {
        case status, matches
        case fetchErrors = "fetch_errors"
    }

    struct Match: Decodable {
        let word: String
        let available: Bool
        let record: DictionaryRecord?
    }

    struct FetchError: Decodable {
        let word: String
        let error: String
    }
}

struct DictionaryRecord: Decodable {
    let schemaVersion: Int
    let query: Query
    let source: Source
    let article: Article
    let headwords: [Headword]
    let mentionedForms: [String]
    let links: [Link]

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case query, source, article, headwords
        case mentionedForms = "mentioned_forms"
        case links
    }

    struct Query: Decodable {
        let word: String
        let requestedURL: String
        let matchType: String
        let matchedHeadwordIndex: Int?

        enum CodingKeys: String, CodingKey {
            case word
            case requestedURL = "requested_url"
            case matchType = "match_type"
            case matchedHeadwordIndex = "matched_headword_index"
        }
    }

    struct Source: Decodable {
        let provider: String
        let collection: String
        let attribution: String?
        let canonicalURL: String
        let citation: String?
        let tdm: TDM?

        enum CodingKeys: String, CodingKey {
            case provider, collection, attribution, citation, tdm
            case canonicalURL = "canonical_url"
        }
    }

    struct TDM: Decodable {
        let reservation: String
        let policyURL: String

        enum CodingKeys: String, CodingKey {
            case reservation
            case policyURL = "policy_url"
        }
    }

    struct Article: Decodable {
        let title: String
        let text: String
    }

    struct Headword: Decodable {
        let word: String
        let grammar: Grammar
        let text: String
    }

    struct Grammar: Decodable {
        let label: String
        let description: String?
        let partOfSpeech: String
        let gender: String?

        enum CodingKeys: String, CodingKey {
            case label, description, gender
            case partOfSpeech = "part_of_speech"
        }
    }

    struct Link: Decodable {
        let text: String
        let url: String
        let relation: String
    }
}

enum LookupError: LocalizedError {
    case emptyClipboard
    case indexNotFound
    case wordNotFound(String)
    case recordUnavailable(String, String?)
    case invalidManifest(Int)
    case malformedRecord(String)

    var errorDescription: String? {
        switch self {
        case .emptyClipboard:
            return "The clipboard does not contain a word."
        case .indexNotFound:
            return "No dictionary index was found. Set WOERTERBUCH_INDEX to the directory containing manifest.jsonl."
        case let .wordNotFound(word):
            return "“\(word)” is not present in the dictionary manifest."
        case let .recordUnavailable(word, detail):
            if let detail, !detail.isEmpty {
                return "The entry for “\(word)” has not been downloaded.\n\n\(detail)"
            }
            return "The entry for “\(word)” has not been downloaded."
        case let .invalidManifest(line):
            return "The dictionary manifest contains invalid JSON on line \(line)."
        case let .malformedRecord(path):
            return "The dictionary record is malformed: \(path)"
        }
    }
}

struct DictionaryIndex {
    let root: URL

    private var manifestURL: URL {
        root.appendingPathComponent("manifest.jsonl")
    }

    func lookup(_ requestedWord: String) throws -> DictionaryRecord {
        let word = requestedWord.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !word.isEmpty else {
            throw LookupError.emptyClipboard
        }

        let data = try Data(contentsOf: manifestURL)
        guard let contents = String(data: data, encoding: .utf8) else {
            throw LookupError.invalidManifest(1)
        }

        let decoder = JSONDecoder()
        var insensitiveMatch: ManifestEntry?
        for (offset, line) in contents.split(whereSeparator: \Character.isNewline).enumerated() {
            guard let lineData = String(line).data(using: .utf8),
                  let entry = try? decoder.decode(ManifestEntry.self, from: lineData) else {
                throw LookupError.invalidManifest(offset + 1)
            }
            if entry.word == word {
                return try load(entry)
            }
            if insensitiveMatch == nil && normalized(entry.word) == normalized(word) {
                insensitiveMatch = entry
            }
        }
        if let insensitiveMatch {
            return try load(insensitiveMatch)
        }
        throw LookupError.wordNotFound(word)
    }

    private func load(_ entry: ManifestEntry) throws -> DictionaryRecord {
        let recordURL = root.appendingPathComponent(entry.file)
        guard FileManager.default.fileExists(atPath: recordURL.path) else {
            let errorURL = URL(fileURLWithPath: recordURL.path + ".error.log")
            let detail = try? String(contentsOf: errorURL, encoding: .utf8)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw LookupError.recordUnavailable(entry.word, detail)
        }
        do {
            return try JSONDecoder().decode(
                DictionaryRecord.self,
                from: Data(contentsOf: recordURL)
            )
        } catch {
            throw LookupError.malformedRecord(recordURL.path)
        }
    }

    private func normalized(_ word: String) -> String {
        word.precomposedStringWithCanonicalMapping
            .folding(options: [.caseInsensitive], locale: Locale(identifier: "de_DE"))
    }
}

func lookupOnDemand(_ word: String, indexRoot: URL) throws -> DictionaryRecord? {
    let projectRoot = indexRoot.deletingLastPathComponent()
    let command = projectRoot.appendingPathComponent("bin/lookup-word")
    let database = indexRoot.appendingPathComponent("lookup.sqlite3")
    guard FileManager.default.isExecutableFile(atPath: command.path),
          FileManager.default.fileExists(atPath: database.path) else {
        return nil
    }

    let output = Pipe()
    let diagnostics = Pipe()
    let process = Process()
    process.executableURL = command
    process.arguments = [word, database.path]
    process.currentDirectoryURL = projectRoot
    process.standardOutput = output
    process.standardError = diagnostics
    var environment = ProcessInfo.processInfo.environment
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    process.environment = environment

    do {
        try process.run()
    } catch {
        return nil
    }
    let outputData = output.fileHandleForReading.readDataToEndOfFile()
    let diagnosticData = diagnostics.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()

    guard let response = try? JSONDecoder().decode(LookupResponse.self, from: outputData) else {
        let message = String(data: diagnosticData, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        throw LookupError.recordUnavailable(word, message)
    }
    if let record = response.matches.compactMap(\.record).first {
        return record
    }
    if response.status == "not_found" {
        throw LookupError.wordNotFound(word)
    }
    let details = response.fetchErrors?
        .map { $0.word.isEmpty ? $0.error : "\($0.word): \($0.error)" }
        .joined(separator: "\n")
    throw LookupError.recordUnavailable(word, details)
}

func render(_ record: DictionaryRecord) -> String {
    var sections: [String] = []
    sections.append("SCHEMA\nVersion: \(record.schemaVersion)")

    var query = [
        "Word: \(record.query.word)",
        "Requested URL: \(record.query.requestedURL)",
        "Match type: \(record.query.matchType)",
    ]
    query.append("Matched headword index: \(record.query.matchedHeadwordIndex.map(String.init) ?? "none")")
    sections.append("QUERY\n" + query.joined(separator: "\n"))

    var source = [
        "Provider: \(record.source.provider)",
        "Collection: \(record.source.collection)",
        "Canonical URL: \(record.source.canonicalURL)",
    ]
    if let attribution = record.source.attribution {
        source.append("Attribution: \(attribution)")
    }
    if let citation = record.source.citation {
        source.append("Citation: \(citation)")
    }
    if let tdm = record.source.tdm {
        source.append("TDM reservation: \(tdm.reservation)")
        source.append("TDM policy: \(tdm.policyURL)")
    }
    sections.append("SOURCE\n" + source.joined(separator: "\n"))

    sections.append("ARTICLE\nTitle: \(record.article.title)\n\n\(record.article.text)")

    let headwords = record.headwords.enumerated().map { index, headword in
        var grammar = [
            "label=\(headword.grammar.label)",
            "part_of_speech=\(headword.grammar.partOfSpeech)",
        ]
        if let description = headword.grammar.description {
            grammar.append("description=\(description)")
        }
        if let gender = headword.grammar.gender {
            grammar.append("gender=\(gender)")
        }
        return "[\(index)] \(headword.word)\nGrammar: \(grammar.joined(separator: ", "))\n\(headword.text)"
    }
    sections.append("HEADWORDS\n" + headwords.joined(separator: "\n\n"))

    let forms = record.mentionedForms.isEmpty ? "none" : record.mentionedForms.joined(separator: ", ")
    sections.append("MENTIONED FORMS\n\(forms)")

    let links = record.links.isEmpty
        ? "none"
        : record.links.map { "\($0.text) [\($0.relation)]\n\($0.url)" }.joined(separator: "\n\n")
    sections.append("LINKS\n\(links)")
    return sections.joined(separator: "\n\n────────────────────\n\n")
}

func renderAttributed(_ record: DictionaryRecord) -> NSAttributedString {
    let result = NSMutableAttributedString()
    let bodyFont = NSFont.systemFont(ofSize: 14)
    let labelFont = NSFont.systemFont(ofSize: 12, weight: .semibold)
    let valueFont = NSFont.systemFont(ofSize: 12)
    let secondaryColor = NSColor.secondaryLabelColor

    func append(_ text: String, font: NSFont, color: NSColor = .textColor,
                spacing: CGFloat = 3) {
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineSpacing = spacing
        result.append(NSAttributedString(string: text, attributes: [
            .font: font,
            .foregroundColor: color,
            .paragraphStyle: paragraph,
        ]))
    }

    func heading(_ text: String) {
        if result.length > 0 {
            append("\n", font: bodyFont)
        }
        let paragraph = NSMutableParagraphStyle()
        paragraph.paragraphSpacingBefore = 8
        paragraph.paragraphSpacing = 8
        result.append(NSAttributedString(string: text + "\n", attributes: [
            .font: NSFont.systemFont(ofSize: 18, weight: .semibold),
            .foregroundColor: NSColor.labelColor,
            .paragraphStyle: paragraph,
        ]))
    }

    func field(_ label: String, _ value: String?) {
        guard let value, !value.isEmpty else {
            return
        }
        append(label + "  ", font: labelFont, color: secondaryColor, spacing: 1)
        append(value + "\n", font: valueFont, spacing: 1)
    }

    func linkedField(_ label: String, _ value: String?) {
        guard let value, let url = URL(string: value) else {
            return
        }
        append(label + "  ", font: labelFont, color: secondaryColor, spacing: 1)
        result.append(NSAttributedString(string: value + "\n", attributes: [
            .font: valueFont,
            .link: url,
            .foregroundColor: NSColor.linkColor,
            .underlineStyle: NSUnderlineStyle.single.rawValue,
        ]))
    }

    heading("Article")
    append(record.article.text + "\n", font: bodyFont, spacing: 4)

    heading("Headwords")
    for (index, headword) in record.headwords.enumerated() {
        if index > 0 {
            append("\n", font: bodyFont)
        }
        append(headword.word + "\n", font: NSFont.systemFont(ofSize: 15, weight: .semibold))
        let grammar = [
            headword.grammar.label,
            headword.grammar.description,
            headword.grammar.partOfSpeech,
            headword.grammar.gender,
        ].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: "  ·  ")
        append(grammar + "\n", font: NSFont.systemFont(ofSize: 12), color: secondaryColor)
        append(headword.text + "\n", font: bodyFont, spacing: 4)
    }

    heading("Query")
    field("Word", record.query.word)
    field("Match", record.query.matchType.replacingOccurrences(of: "_", with: " "))
    field("Headword index", record.query.matchedHeadwordIndex.map(String.init) ?? "none")
    linkedField("Requested URL", record.query.requestedURL)

    heading("Source")
    field("Provider", record.source.provider)
    field("Collection", record.source.collection)
    field("Attribution", record.source.attribution)
    field("Citation", record.source.citation)
    linkedField("Canonical URL", record.source.canonicalURL)
    if let tdm = record.source.tdm {
        field("TDM reservation", tdm.reservation)
        linkedField("TDM policy", tdm.policyURL)
    }

    heading("Mentioned forms")
    append(
        (record.mentionedForms.isEmpty ? "None" : record.mentionedForms.joined(separator: ", ")) + "\n",
        font: bodyFont
    )

    heading("Cross-references")
    if record.links.isEmpty {
        append("None\n", font: bodyFont)
    } else {
        for link in record.links {
            append(link.text + "  ", font: labelFont)
            if let url = URL(string: link.url) {
                result.append(NSAttributedString(string: link.url + "\n", attributes: [
                    .font: valueFont,
                    .link: url,
                    .foregroundColor: NSColor.linkColor,
                    .underlineStyle: NSUnderlineStyle.single.rawValue,
                ]))
            }
        }
    }
    return result
}

func discoverIndex(explicitPath: String?) throws -> URL {
    let fileManager = FileManager.default
    if let explicitPath {
        let url = URL(fileURLWithPath: explicitPath, isDirectory: true)
        if fileManager.fileExists(atPath: url.appendingPathComponent("manifest.jsonl").path) {
            return url
        }
        throw LookupError.indexNotFound
    }
    if let environmentPath = ProcessInfo.processInfo.environment["WOERTERBUCH_INDEX"] {
        let url = URL(fileURLWithPath: environmentPath, isDirectory: true)
        if fileManager.fileExists(atPath: url.appendingPathComponent("manifest.jsonl").path) {
            return url
        }
    }

    var candidates: [URL] = []
    let workingDirectory = URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true)
    candidates.append(workingDirectory.appendingPathComponent("word_index", isDirectory: true))

    var ancestor = Bundle.main.bundleURL.deletingLastPathComponent()
    for _ in 0..<5 {
        candidates.append(ancestor.appendingPathComponent("word_index", isDirectory: true))
        ancestor.deleteLastPathComponent()
    }
    for candidate in candidates where
        fileManager.fileExists(atPath: candidate.appendingPathComponent("manifest.jsonl").path) {
        return candidate
    }
    throw LookupError.indexNotFound
}

final class DialogController: NSObject, NSWindowDelegate {
    let panel: NSPanel

    init(title: String, summary: String, detail: NSAttributedString?) {
        let visibleSize = NSScreen.main?.visibleFrame.size ?? NSSize(width: 1440, height: 900)
        let recordSize = NSSize(
            width: min(1400, visibleSize.width * 0.92),
            height: min(1100, visibleSize.height * 0.90)
        )
        let size = detail == nil ? NSSize(width: 520, height: 220) : recordSize
        panel = NSPanel(
            contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.titled, .closable, .resizable],
            backing: .buffered,
            defer: false
        )
        super.init()
        panel.delegate = self
        panel.title = title
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isReleasedWhenClosed = false
        panel.minSize = detail == nil
            ? NSSize(width: 440, height: 190)
            : NSSize(width: 680, height: 500)

        let content = NSView()
        panel.contentView = content

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 26, weight: .semibold)
        titleLabel.maximumNumberOfLines = 2
        titleLabel.lineBreakMode = .byWordWrapping

        let summaryLabel = NSTextField(wrappingLabelWithString: summary)
        summaryLabel.font = .systemFont(ofSize: 13)
        summaryLabel.textColor = .secondaryLabelColor

        let dismissButton = NSButton(title: "Dismiss", target: nil, action: nil)
        dismissButton.bezelStyle = .rounded
        dismissButton.keyEquivalent = "\r"

        for view in [titleLabel, summaryLabel, dismissButton] {
            view.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(view)
        }
        dismissButton.target = self
        dismissButton.action = #selector(dismiss)

        var constraints = [
            titleLabel.topAnchor.constraint(equalTo: content.topAnchor, constant: 22),
            titleLabel.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 24),
            titleLabel.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24),
            summaryLabel.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 5),
            summaryLabel.leadingAnchor.constraint(equalTo: titleLabel.leadingAnchor),
            summaryLabel.trailingAnchor.constraint(equalTo: titleLabel.trailingAnchor),
            dismissButton.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24),
            dismissButton.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -18),
            dismissButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 96),
        ]

        if let detail {
            let scrollView = NSScrollView()
            scrollView.translatesAutoresizingMaskIntoConstraints = false
            scrollView.hasVerticalScroller = true
            scrollView.hasHorizontalScroller = false
            scrollView.autohidesScrollers = true
            scrollView.borderType = .bezelBorder
            scrollView.drawsBackground = true
            scrollView.backgroundColor = .textBackgroundColor

            let textView = NSTextView(frame: NSRect(
                x: 0,
                y: 0,
                width: size.width - 48,
                height: size.height - 160
            ))
            textView.isEditable = false
            textView.isSelectable = true
            textView.isRichText = true
            textView.drawsBackground = true
            textView.backgroundColor = .textBackgroundColor
            textView.textContainerInset = NSSize(width: 18, height: 16)
            textView.isHorizontallyResizable = false
            textView.isVerticallyResizable = true
            textView.minSize = NSSize(width: 0, height: 0)
            textView.maxSize = NSSize(
                width: CGFloat.greatestFiniteMagnitude,
                height: CGFloat.greatestFiniteMagnitude
            )
            textView.autoresizingMask = [.width]
            textView.textContainer?.widthTracksTextView = true
            textView.textContainer?.heightTracksTextView = false
            textView.linkTextAttributes = [
                .foregroundColor: NSColor.linkColor,
                .underlineStyle: NSUnderlineStyle.single.rawValue,
            ]
            textView.textStorage?.setAttributedString(detail)
            scrollView.documentView = textView
            content.addSubview(scrollView)

            constraints += [
                scrollView.topAnchor.constraint(equalTo: summaryLabel.bottomAnchor, constant: 18),
                scrollView.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 24),
                scrollView.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24),
                scrollView.bottomAnchor.constraint(equalTo: dismissButton.topAnchor, constant: -18),
            ]
        } else {
            constraints.append(summaryLabel.bottomAnchor.constraint(lessThanOrEqualTo: dismissButton.topAnchor, constant: -18))
        }
        NSLayoutConstraint.activate(constraints)
    }

    @objc func dismiss() {
        NSApp.stopModal()
        panel.orderOut(nil)
    }

    func windowWillClose(_ notification: Notification) {
        NSApp.stopModal()
    }

    func run() {
        panel.center()
        panel.makeKeyAndOrderFront(nil)
        NSApp.runModal(for: panel)
        panel.orderOut(nil)
    }
}

func showDialog(title: String, summary: String, detail: NSAttributedString?) {
    let application = NSApplication.shared
    application.setActivationPolicy(.accessory)
    application.activate(ignoringOtherApps: true)
    DialogController(title: title, summary: summary, detail: detail).run()
}

struct Arguments {
    var indexPath: String?
    var lookupWord: String?
    var printOutput = false
}

func parseArguments() -> Arguments {
    var result = Arguments()
    var index = 1
    let arguments = CommandLine.arguments
    while index < arguments.count {
        switch arguments[index] {
        case "--index" where index + 1 < arguments.count:
            index += 1
            result.indexPath = arguments[index]
        case "--lookup" where index + 1 < arguments.count:
            index += 1
            result.lookupWord = arguments[index]
        case "--print":
            result.printOutput = true
        case "--help", "-h":
            print("usage: Woerterbuch [--index DIR] [--lookup WORD] [--print]")
            exit(0)
        default:
            fputs("Woerterbuch: unknown or incomplete option: \(arguments[index])\n", stderr)
            exit(2)
        }
        index += 1
    }
    return result
}

let arguments = parseArguments()

do {
    let indexURL = try discoverIndex(explicitPath: arguments.indexPath)
    let clipboardWord = NSPasteboard.general.string(forType: .string)
    let word = arguments.lookupWord ?? clipboardWord ?? ""
    let record: DictionaryRecord
    do {
        record = try DictionaryIndex(root: indexURL).lookup(word)
    } catch let lookupError as LookupError {
        switch lookupError {
        case .recordUnavailable, .wordNotFound:
            record = try lookupOnDemand(word, indexRoot: indexURL) ?? {
                throw lookupError
            }()
        default:
            throw lookupError
        }
    }
    if arguments.printOutput {
        print(render(record))
    } else {
        showDialog(
            title: record.article.title,
            summary: "\(word.trimmingCharacters(in: .whitespacesAndNewlines))  ·  \(record.query.matchType.replacingOccurrences(of: "_", with: " "))  ·  schema \(record.schemaVersion)",
            detail: renderAttributed(record)
        )
    }
} catch {
    let message = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    if arguments.printOutput {
        fputs("Woerterbuch: \(message)\n", stderr)
        exit(1)
    } else {
        showDialog(title: "Wörterbuch", summary: message, detail: nil)
    }
}

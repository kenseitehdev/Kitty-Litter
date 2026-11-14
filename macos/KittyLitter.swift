import AppKit
import SwiftUI

struct CleanupCandidate: Identifiable, Hashable {
    let id: String
    let url: URL
    let bytes: Int64
    let modified: Date
    let category: String

    var displayName: String { url.lastPathComponent }
}

struct QuarantineRecord: Codable, Identifiable {
    let id: UUID
    let originalPath: String
    let bytes: Int64
    let modified: Date
    let category: String
    let quarantinedAt: Date

    var payloadURL: URL { KittyPaths.quarantine.appendingPathComponent(id.uuidString).appendingPathComponent("payload") }
    var originalURL: URL { URL(fileURLWithPath: originalPath) }
    var displayName: String { originalURL.lastPathComponent }
}

enum KittyPaths {
    static let support = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/Kitty Litter", isDirectory: true)
    static let quarantine = support.appendingPathComponent("Quarantine", isDirectory: true)
    static let records = quarantine.appendingPathComponent("records.json")
}

@MainActor
final class CleanupStore: ObservableObject {
    @Published private(set) var candidates: [CleanupCandidate] = []
    @Published private(set) var quarantineItems: [QuarantineRecord] = []
    @Published var selectedCandidates = Set<String>()
    @Published var selectedQuarantine = Set<UUID>()
    @Published private(set) var lastScan: Date?
    @Published var message = "Ready to scan"

    private let fm = FileManager.default
    private let maxCandidates = 5_000
    private var scanTimer: Timer?

    init() {
        loadRecords()
        scanTimer = Timer.scheduledTimer(withTimeInterval: 6 * 60 * 60, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.scan() }
        }
    }

    var selectedBytes: Int64 {
        candidates.filter { selectedCandidates.contains($0.id) }.reduce(0) { $0 + $1.bytes }
    }

    var availableBytes: Int64 {
        let home = fm.homeDirectoryForCurrentUser
        let values = try? home.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return Int64(values?.volumeAvailableCapacityForImportantUsage ?? 0)
    }

    func scan() {
        let home = fm.homeDirectoryForCurrentUser
        let now = Date()
        let cacheCutoff = now.addingTimeInterval(-30 * 24 * 60 * 60)
        let downloadCutoff = now.addingTimeInterval(-60 * 24 * 60 * 60)
        var found: [CleanupCandidate] = []
        collect(from: home.appendingPathComponent("Library/Caches"), category: "Cache", cutoff: cacheCutoff,
                minimumBytes: 1_048_576, extensions: nil, into: &found)
        collect(from: home.appendingPathComponent("Library/Logs"), category: "Log", cutoff: cacheCutoff,
                minimumBytes: 1_048_576, extensions: nil, into: &found)
        collect(from: home.appendingPathComponent("Downloads"), category: "Old download", cutoff: downloadCutoff,
                minimumBytes: 50 * 1_048_576, extensions: ["dmg", "pkg", "zip"], into: &found)
        candidates = found.sorted { $0.bytes > $1.bytes }
        selectedCandidates = []
        lastScan = now
        message = "Found \(candidates.count) reviewable items"
    }

    private func collect(from root: URL, category: String, cutoff: Date, minimumBytes: Int64,
                         extensions: Set<String>?, into found: inout [CleanupCandidate]) {
        guard found.count < maxCandidates,
              let enumerator = fm.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey], options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { return }
        for case let url as URL in enumerator {
            if found.count >= maxCandidates { break }
            guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey]),
                  values.isSymbolicLink != true,
                  let modified = values.contentModificationDate, modified < cutoff else { continue }
            if let extensions, !extensions.contains(url.pathExtension.lowercased()) { continue }
            let bytes: Int64
            if values.isRegularFile == true, let size = values.fileSize {
                bytes = Int64(size)
            } else if extensions?.contains("pkg") == true, url.pathExtension.lowercased() == "pkg",
                      (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                bytes = directorySize(at: url)
            } else { continue }
            guard bytes >= minimumBytes else { continue }
            found.append(CleanupCandidate(id: url.path, url: url, bytes: bytes, modified: modified, category: category))
        }
    }

    private func directorySize(at directory: URL) -> Int64 {
        guard let enumerator = fm.enumerator(at: directory, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey], options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { return 0 }
        var total: Int64 = 0
        for case let child as URL in enumerator {
            guard let values = try? child.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]),
                  values.isRegularFile == true, values.isSymbolicLink != true else { continue }
            total += Int64(values.fileSize ?? 0)
        }
        return total
    }

    func quarantineSelected() {
        do { try fm.createDirectory(at: KittyPaths.quarantine, withIntermediateDirectories: true) }
        catch { message = "Could not create quarantine: \(error.localizedDescription)"; return }
        var moved = Set<String>()
        for item in candidates where selectedCandidates.contains(item.id) {
            do {
                let id = UUID()
                let directory = KittyPaths.quarantine.appendingPathComponent(id.uuidString, isDirectory: true)
                try fm.createDirectory(at: directory, withIntermediateDirectories: true)
                let payload = directory.appendingPathComponent("payload")
                try fm.moveItem(at: item.url, to: payload)
                let record = QuarantineRecord(id: id, originalPath: item.url.path, bytes: item.bytes,
                                              modified: item.modified, category: item.category, quarantinedAt: Date())
                quarantineItems.append(record)
                moved.insert(item.id)
            } catch { message = "Could not quarantine \(item.displayName): \(error.localizedDescription)" }
        }
        candidates.removeAll { moved.contains($0.id) }
        selectedCandidates.subtract(moved)
        do { try saveRecords() } catch { message = "Could not save quarantine index: \(error.localizedDescription)"; return }
        message = "Moved \(moved.count) item(s) to quarantine"
    }

    func restoreSelected() {
        var restored = Set<UUID>()
        for item in quarantineItems where selectedQuarantine.contains(item.id) {
            guard !fm.fileExists(atPath: item.originalURL.path) else {
                message = "Restore skipped: a file already exists at \(item.originalPath)"
                continue
            }
            do {
                try fm.createDirectory(at: item.originalURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fm.moveItem(at: item.payloadURL, to: item.originalURL)
                try fm.removeItem(at: item.payloadURL.deletingLastPathComponent())
                restored.insert(item.id)
            } catch { message = "Could not restore \(item.displayName): \(error.localizedDescription)" }
        }
        quarantineItems.removeAll { restored.contains($0.id) }
        selectedQuarantine.subtract(restored)
        persistRecords()
        message = restored.isEmpty ? message : "Restored \(restored.count) item(s)"
    }

    func releaseSelected() {
        let chosen = quarantineItems.filter { selectedQuarantine.contains($0.id) }
        guard !chosen.isEmpty else { return }
        let alert = NSAlert()
        alert.messageText = "Permanently release \(chosen.count) item(s)?"
        alert.informativeText = "This deletes the selected quarantined files and cannot be undone."
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Release Permanently")
        alert.buttons[0].keyEquivalent = "\u{1b}"
        guard alert.runModal() == .alertSecondButtonReturn else { return }
        var removed = Set<UUID>()
        for item in chosen {
            do {
                try fm.removeItem(at: item.payloadURL.deletingLastPathComponent())
                removed.insert(item.id)
            } catch { message = "Could not release \(item.displayName): \(error.localizedDescription)" }
        }
        quarantineItems.removeAll { removed.contains($0.id) }
        selectedQuarantine.subtract(removed)
        persistRecords()
        message = "Permanently released \(removed.count) item(s)"
    }

    private func loadRecords() {
        guard let data = try? Data(contentsOf: KittyPaths.records),
              let records = try? JSONDecoder().decode([QuarantineRecord].self, from: data) else { return }
        quarantineItems = records.filter { fm.fileExists(atPath: $0.payloadURL.path) }
    }

    private func persistRecords() {
        do { try saveRecords() } catch { message = "Could not save quarantine index: \(error.localizedDescription)" }
    }

    private func saveRecords() throws {
        try fm.createDirectory(at: KittyPaths.quarantine, withIntermediateDirectories: true)
        let data = try JSONEncoder().encode(quarantineItems)
        try data.write(to: KittyPaths.records, options: .atomic)
    }
}

struct ReviewView: View {
    @EnvironmentObject private var store: CleanupStore

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Kitty Litter").font(.title2.bold())
                    Text(store.availableBytes < 20_000_000_000 ? "Low disk space · \(ByteCountFormatter.string(fromByteCount: store.availableBytes, countStyle: .file)) available" : "\(ByteCountFormatter.string(fromByteCount: store.availableBytes, countStyle: .file)) available")
                        .font(.caption).foregroundStyle(store.availableBytes < 20_000_000_000 ? .orange : .secondary)
                }
                Spacer()
                Button("Scan Now") { store.scan() }
            }
            TabView {
                candidateList
                    .tabItem { Label("Candidates", systemImage: "sparkles") }
                quarantineList
                    .tabItem { Label("Quarantine", systemImage: "shippingbox") }
            }
            HStack {
                Text(store.message).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Text("Selected: \(ByteCountFormatter.string(fromByteCount: store.selectedBytes, countStyle: .file))").font(.caption)
                Button("Pound into Quarantine") { store.quarantineSelected() }
                    .disabled(store.selectedCandidates.isEmpty)
            }
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 460)
        .onAppear { if store.lastScan == nil { store.scan() } }
    }

    private var candidateList: some View {
        List {
            if store.candidates.isEmpty {
                Text("No candidates yet. Scan checks old, large files in your caches, logs, and Downloads.")
                    .foregroundStyle(.secondary)
            }
            ForEach(store.candidates) { item in
                Toggle(isOn: selectionBinding(item.id)) {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(item.displayName).lineLimit(1)
                            Text("\(item.category) · \(item.url.path) · older than threshold")
                                .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        }
                        Spacer()
                        Text(ByteCountFormatter.string(fromByteCount: item.bytes, countStyle: .file))
                            .font(.caption.monospacedDigit())
                    }
                }.toggleStyle(.checkbox)
            }
        }
    }

    private var quarantineList: some View {
        VStack(alignment: .leading) {
            List {
                if store.quarantineItems.isEmpty { Text("Quarantine is empty").foregroundStyle(.secondary) }
                ForEach(store.quarantineItems) { item in
                    Toggle(isOn: quarantineBinding(item.id)) {
                        HStack {
                            VStack(alignment: .leading) {
                                Text(item.displayName)
                                Text("\(item.category) · originally \(item.originalPath)")
                                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            Spacer()
                            Text(ByteCountFormatter.string(fromByteCount: item.bytes, countStyle: .file)).font(.caption)
                        }
                    }.toggleStyle(.checkbox)
                }
            }
            HStack {
                Spacer()
                Button("Restore") { store.restoreSelected() }.disabled(store.selectedQuarantine.isEmpty)
                Button("Release Permanently…") { store.releaseSelected() }.disabled(store.selectedQuarantine.isEmpty)
            }.padding(.horizontal, 8).padding(.bottom, 4)
        }
    }

    private func selectionBinding(_ id: String) -> Binding<Bool> {
        Binding(get: { store.selectedCandidates.contains(id) }, set: { value in
            if value { store.selectedCandidates.insert(id) } else { store.selectedCandidates.remove(id) }
        })
    }

    private func quarantineBinding(_ id: UUID) -> Binding<Bool> {
        Binding(get: { store.selectedQuarantine.contains(id) }, set: { value in
            if value { store.selectedQuarantine.insert(id) } else { store.selectedQuarantine.remove(id) }
        })
    }
}

@MainActor
final class KittyLitterAppDelegate: NSObject, NSApplicationDelegate {
    private let store = CleanupStore()
    private var statusItem: NSStatusItem!
    private var reviewWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        store.scan()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = makeMenuBarIcon()
            button.toolTip = "Kitty Litter · Disk cleanup review"
        }

        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "Open Review Window", action: #selector(openReview), keyEquivalent: "o"))
        menu.addItem(NSMenuItem(title: "Scan Now", action: #selector(scanNow), keyEquivalent: "r"))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Kitty Litter", action: #selector(quit), keyEquivalent: "q"))
        for item in menu.items { item.target = self }
        statusItem.menu = menu
    }

    @objc private func openReview() {
        if reviewWindow == nil {
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 540),
                                  styleMask: [.titled, .closable, .miniaturizable, .resizable],
                                  backing: .buffered, defer: false)
            window.title = "Kitty Litter"
            window.contentView = NSHostingView(rootView: ReviewView().environmentObject(store))
            window.center()
            reviewWindow = window
        }
        NSApp.activate(ignoringOtherApps: true)
        reviewWindow?.makeKeyAndOrderFront(nil)
    }

    @objc private func scanNow() { store.scan() }
    @objc private func quit() { NSApp.terminate(nil) }

    private func makeMenuBarIcon() -> NSImage {
        let size = NSSize(width: 20, height: 18)
        let image = NSImage(size: size, flipped: false) { _ in
            NSColor.black.setFill()
            NSColor.black.setStroke()

            // Open litter tray, drawn as a bold silhouette with a clear rim.
            let tray = NSBezierPath()
            tray.lineWidth = 1.7
            tray.lineCapStyle = .round
            tray.lineJoinStyle = .round
            tray.move(to: NSPoint(x: 2.2, y: 11.5))
            tray.line(to: NSPoint(x: 17.8, y: 11.5))
            tray.line(to: NSPoint(x: 16.1, y: 3.4))
            tray.curve(to: NSPoint(x: 13.5, y: 1.8), controlPoint1: NSPoint(x: 15.9, y: 2.4), controlPoint2: NSPoint(x: 14.8, y: 1.8))
            tray.line(to: NSPoint(x: 6.5, y: 1.8))
            tray.curve(to: NSPoint(x: 3.9, y: 3.4), controlPoint1: NSPoint(x: 5.2, y: 1.8), controlPoint2: NSPoint(x: 4.1, y: 2.4))
            tray.close()
            tray.stroke()

            // A few large litter grains remain visible at 1x menu-bar scale.
            for point in [NSPoint(x: 6.0, y: 5.0), NSPoint(x: 9.1, y: 4.2), NSPoint(x: 13.1, y: 5.2)] {
                NSBezierPath(ovalIn: NSRect(x: point.x, y: point.y, width: 1.4, height: 1.4)).fill()
            }

            // Scoop bowl and handle sit above the tray rim so the tool reads clearly.
            let bowl = NSBezierPath(ovalIn: NSRect(x: 4.2, y: 10.4, width: 5.0, height: 4.1))
            bowl.lineWidth = 1.5
            bowl.stroke()
            for point in [NSPoint(x: 5.7, y: 11.7), NSPoint(x: 7.7, y: 11.7), NSPoint(x: 6.7, y: 13.0)] {
                NSBezierPath(ovalIn: NSRect(x: point.x, y: point.y, width: 0.65, height: 0.65)).fill()
            }
            let handle = NSBezierPath()
            handle.lineWidth = 2.0
            handle.lineCapStyle = .round
            handle.move(to: NSPoint(x: 8.7, y: 13.6))
            handle.line(to: NSPoint(x: 14.6, y: 17.0))
            handle.stroke()
            return true
        }
        image.isTemplate = true
        return image
    }
}

@main
enum KittyLitterMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = KittyLitterAppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        app.run()
    }
}

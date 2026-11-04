import AppKit
import Foundation

private let directions = ["Right", "Left", "Up", "Down"]
// Xbox Adaptive Controller's right USB port, in HID slot order.
private let buttons = ["Off", "View", "Menu", "RS · Right stick press", "RB · Right bumper", "X", "Y",
                       "X1", "X2", "LS · Left stick press", "LB · Left bumper", "A", "B"]
private let menuOrder = [0, 11, 12, 5, 6, 10, 4, 9, 3, 1, 2, 7, 8]
private struct EditorError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
    init(_ text: String) { message = text }
}
private struct Saved: Decodable {
    let revision: UInt32
    let slots: [Int]
    let storage: Int
    let result: Int
    init(_ output: String) throws {
        self = try JSONDecoder().decode(Self.self, from: Data(output.utf8))
        guard slots.count == 4, slots.allSatisfy({ buttons.indices.contains($0) }),
              (0...3).contains(storage), (0...6).contains(result),
              storage < 2 || slots == [0,0,0,0] else { throw EditorError("The Pico sent an unreadable assignment. Reload to try again.") }
    }
    func confirms(_ wanted: [Int], after before: UInt32) -> Bool {
        before < UInt32.max && revision == before + 1 && slots == wanted && storage == 1 && result == 2
    }
}
private struct Session {
    let location: String
    let saved: Saved
}
private enum Wired {
    typealias Run = ([String]) throws -> String
    static func location(_ probe: String) throws -> String {
        guard let start = probe.range(of: "Mac USB location ")?.upperBound else { throw EditorError("The Pico's USB location could not be verified.") }
        let token = String(probe[start...].prefix { $0 != "." && !$0.isWhitespace })
        guard token.hasPrefix("0x"), token.count > 2, token.count <= 10,
              token.dropFirst(2).allSatisfy({ $0.isHexDigit }), let n=UInt32(token.dropFirst(2),radix:16), n != 0 else {
            throw EditorError("The Pico's USB location could not be verified.")
        }
        return token
    }
    static func read(_ run: Run) throws -> Session {
        let id = try location(run(["--probe"]))
        return try Session(location:id, saved:Saved(run(["--get-nudges",id])))
    }
    static func save(_ session: Session, _ slots: [Int], _ run: Run) throws -> Session {
        guard slots.count == 4, slots.allSatisfy({ buttons.indices.contains($0) }) else { throw EditorError("Choose a button for all four directions.") }
        let args = ["--set-nudges",session.location,String(session.saved.revision)] + slots.map(String.init)
        let updated = try Saved(run(args))
        guard updated.confirms(slots,after:session.saved.revision) else { throw EditorError("Save could not be verified. Reload the current settings.") }
        return Session(location:session.location,saved:updated)
    }
    static func command(_ args: [String]) throws -> String {
        let url = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/omni-maintenance")
        let process=Process(), output=Pipe()
        process.executableURL=url; process.arguments=args; process.standardOutput=output; process.standardError=output
        process.standardInput=FileHandle.nullDevice
        try process.run()
        let data=output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let text=String(decoding:data,as:UTF8.self).trimmingCharacters(in:.whitespacesAndNewlines)
        guard process.terminationStatus==0 else { throw EditorError(text.isEmpty ? "The connection timed out. Connect the Omni Pico to this Mac and click Reload." : text) }
        return text
    }
}

@MainActor private final class Editor: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var menus: [NSPopUpButton]=[]
    private let status=NSTextField(wrappingLabelWithString:"")
    private let reload=NSButton(title:"Reload from Pico",target:nil,action:nil)
    private let save=NSButton(title:"Save to Pico",target:nil,action:nil)
    private var session: Session?
    private var busy=false
    private var saveUncertain=false
    private var selected: [Int] { menus.map { $0.selectedTag() } }
    func applicationDidFinishLaunching(_ notification: Notification) {
        let mainMenu=NSMenu()
        let appItem=NSMenuItem(); let appMenu=NSMenu()
        appMenu.addItem(withTitle:"Quit Omni Nudge Editor",action:#selector(NSApplication.terminate(_:)),keyEquivalent:"q")
        appItem.submenu=appMenu;mainMenu.addItem(appItem);NSApp.mainMenu=mainMenu
        window=NSWindow(contentRect:NSRect(x:0,y:0,width:550,height:490),styleMask:[.titled,.closable,.miniaturizable,.resizable],backing:.buffered,defer:false)
        window.title="Omni Nudge Buttons"; window.delegate=self
        let content=NSView()
        window.contentView=content
        let title=NSTextField(labelWithString:"Chair nudge buttons")
        title.font=NSFont.systemFont(ofSize:26,weight:.semibold)
        let help=NSTextField(wrappingLabelWithString:"Choose what each nudge presses on the Xbox Adaptive Controller’s right USB port. Choose Off to leave a direction unassigned.")
        help.font=NSFont.preferredFont(forTextStyle:.body)
        let grid=NSGridView(views:directions.map { direction in
            let label=NSTextField(labelWithString:direction)
            label.font=NSFont.systemFont(ofSize:16,weight:.medium)
            let menu=NSPopUpButton(frame:.zero,pullsDown:false)
            menu.controlSize = .large; menu.font=NSFont.systemFont(ofSize:16)
            menu.setAccessibilityLabel(direction+" nudge button")
            menu.setAccessibilityHelp("Choose a button for a brief "+direction.lowercased()+" nudge.")
            for index in menuOrder { menu.addItem(withTitle:buttons[index]); menu.lastItem?.tag=index }
            menu.target=self;menu.action=#selector(changed);menus.append(menu)
            menu.widthAnchor.constraint(greaterThanOrEqualToConstant:195).isActive=true
            return [label,menu]
        })
        grid.rowSpacing=9;grid.columnSpacing=20; grid.xPlacement = .leading;grid.yPlacement = .center
        reload.target=self;reload.action=#selector(readPressed);save.target=self;save.action=#selector(savePressed)
        save.keyEquivalent="\r";reload.controlSize = .large;save.controlSize = .large
        let actions=NSStackView(views:[reload,save]);actions.orientation = .horizontal;actions.spacing=12
        status.font=NSFont.preferredFont(forTextStyle:.body);status.setAccessibilityLabel("Connection and save status")
        let stack=NSStackView(views:[title,help,grid,actions,status])
        stack.orientation = .vertical;stack.alignment = .leading;stack.spacing=16
        stack.translatesAutoresizingMaskIntoConstraints=false;content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo:content.leadingAnchor,constant:24),
            stack.trailingAnchor.constraint(equalTo:content.trailingAnchor,constant:-24),
            stack.topAnchor.constraint(equalTo:content.topAnchor,constant:22),
            stack.bottomAnchor.constraint(lessThanOrEqualTo:content.bottomAnchor,constant:-18),
            help.widthAnchor.constraint(equalTo:stack.widthAnchor),
            status.widthAnchor.constraint(equalTo:stack.widthAnchor)
        ])
        for i in menus.indices { menus[i].nextKeyView=i+1<menus.count ? menus[i+1] : reload }
        reload.nextKeyView=save;save.nextKeyView=menus.first
        window.minSize=NSSize(width:480,height:500)
        window.center();window.makeKeyAndOrderFront(nil);NSApp.activate(ignoringOtherApps:true)
        load()
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender:NSApplication)->Bool { true }
    private func tell(_ text:String) {
        status.stringValue=text
        NSAccessibility.post(element:window as Any,notification:.announcementRequested,userInfo:[.announcement:text,.priority:NSAccessibilityPriorityLevel.medium.rawValue])
    }
    private func controls() {
        let editable = !busy && !saveUncertain && session != nil && session?.saved.storage != 3 && session?.saved.result != 1
        menus.forEach { $0.isEnabled=editable }
        reload.isEnabled = !busy
        // A corrupt record may be explicitly replaced by four Off settings.
        save.isEnabled = editable && (selected != session?.saved.slots || session?.saved.storage==2)
        window.isDocumentEdited = editable && selected != session?.saved.slots
    }
    private func show(_ current: Session) {
        session=current;saveUncertain=false
        for (menu,slot) in zip(menus,current.saved.slots) { menu.selectItem(withTag:slot) }
    }
    @objc private func changed() { controls(); if save.isEnabled { tell("Changes are ready. Click Save to Pico to keep them.") } }
    @objc private func readPressed() {
        if window.isDocumentEdited {
            let alert=NSAlert();alert.messageText="Discard unsaved choices and reload?"
            alert.addButton(withTitle:"Keep Editing");alert.addButton(withTitle:"Reload")
            alert.beginSheetModal(for:window){ [weak self] choice in if choice == .alertSecondButtonReturn { self?.load() } }
        } else { load() }
    }
    private func load() {
        guard !busy else { return }
        busy=true;controls();tell("Reading the connected Omni Pico…")
        Task {
            do {
                let current=try await Task.detached { try Wired.read(Wired.command) }.value
                show(current)
                let text: String
                if current.saved.result==1 { text="Another save is in progress. Click Reload when it finishes." }
                else { switch current.saved.storage {
                    case 0: text="Connected. The Pico is using its default buttons."
                    case 1: text="Connected. Saved buttons are shown."
                    case 2: text="Stored settings could not be read. All four buttons are Off. Select the choices you want and save."
                    default: text="The Pico’s settings storage is unavailable. Reconnect the Pico and click Reload."
                } }
                tell(text)
            } catch {
                session=nil; saveUncertain=false; menus.forEach { $0.selectItem(at:-1) }
                tell("Could not load settings. Connect the Omni Pico to this Mac, then click Reload. "+error.localizedDescription)
            }
            busy=false;controls()
        }
    }
    @objc private func savePressed() {
        guard save.isEnabled, let current=session else { return }
        let wanted=selected
        busy=true;controls();tell("Saving and reading back the buttons…")
        Task {
            do {
                let next=try await Task.detached { try Wired.save(current,wanted,Wired.command) }.value
                show(next);tell("Saved and verified on the Pico.")
            } catch { saveUncertain=true;tell(error.localizedDescription+" Reload before another save.") }
            busy=false;controls()
        }
    }
}

@main private struct OmniNudgeEditor {
    static func main() {
        if CommandLine.arguments.dropFirst().elementsEqual(["--self-test"]) { runTests();return }
        let app=NSApplication.shared
        let editor=Editor()
        app.setActivationPolicy(.regular);app.delegate=editor;app.run()
        withExtendedLifetime(editor){}
    }
    static func runTests() {
        var passed=0
        func check(_ result:Bool){ guard result else { fatalError("Editor check failed after \(passed) passes") };passed+=1 }
        let old = #"{"revision":7,"slots":[12,4,5,3],"storage":1,"result":2}"#
        let newer = #"{"revision":8,"slots":[0,4,5,3],"storage":1,"result":2}"#
        var calls=[[String]]()
        let read=try! Wired.read { args in calls.append(args);return args==["--probe"] ? "Maintenance supported. Mac USB location 0x00001000. Probe only." : old }
        check(read.location=="0x00001000" && read.saved.slots==[12,4,5,3])
        check(calls==[["--probe"],["--get-nudges","0x00001000"]])
        let after=try! Wired.save(read,[0,4,5,3]){ args in calls.append(args);return newer }
        check(calls.last==["--set-nudges","0x00001000","7","0","4","5","3"])
        check(after.saved.revision==8 && after.location==read.location)
        check((try? Wired.save(read,[0,4,5,3],{ _ in old }))==nil)
        check((try? Wired.save(read,[0,0,5,3],{ _ in newer }))==nil)
        check((try? Wired.save(read,[13,4,5,3],{ _ in fatalError("invalid sent") }))==nil)
        check((try? Saved(#"{"revision":7,"slots":[12,4,5],"storage":0,"result":0}"#))==nil)
        check((try? Wired.location("Mac USB location 0x00. Probe only."))==nil)
        check(Set(menuOrder)==Set(0...12) && menuOrder.count==13 && buttons[12]=="B" && buttons[4].hasPrefix("RB") && buttons[5]=="X" && buttons[3].hasPrefix("RS"))
        print("PASS: \(passed) editor model and target checks. No UI or hardware opened.")
    }
}

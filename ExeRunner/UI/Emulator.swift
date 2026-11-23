import Foundation

/// C -> Swift output hook. Must not capture anything, so it is a free function.
private func emulatorOutput(_ data: UnsafePointer<CChar>?, _ len: Int, _ stream: Int32, _ user: UnsafeMutableRawPointer?) {
    guard let data, let user, len > 0 else { return }
    let emulator = Unmanaged<Emulator>.fromOpaque(user).takeUnretainedValue()
    let bytes = UnsafeBufferPointer(start: UnsafeRawPointer(data).assumingMemoryBound(to: UInt8.self), count: len)
    let text = String(decoding: bytes, as: UTF8.self)
    DispatchQueue.main.async { emulator.output += text }
}

final class Emulator: ObservableObject {
    @Published var output = ""
    @Published var status = "Choose an .exe to begin"
    @Published var isRunning = false
    @Published var fileName: String?
    @Published var useJIT = true
    @Published var jitWasActive = false
    @Published var hasRun = false

    /// True when this process can currently execute generated code (a JIT enabler / debugger is attached).
    var jitAvailable: Bool { er_jit_available() != 0 }

    private var exeData: Data?
    private var machine: OpaquePointer?
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "exerunner.emulator", qos: .userInitiated)

    func load(url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            exeData = data
            fileName = url.lastPathComponent
            status = "Loaded \(url.lastPathComponent) (\(data.count) bytes)"
            output = ""
        } catch {
            status = "Could not read file: \(error.localizedDescription)"
        }
    }

    func run() {
        guard let data = exeData, !isRunning else { return }
        output = ""
        isRunning = true
        status = "Running…"
        let wantJIT = useJIT
        let context = Unmanaged.passUnretained(self).toOpaque()

        queue.async { [weak self] in
            guard let self else { return }
            guard let m = er_create(emulatorOutput, context) else {
                self.finish(status: "Could not reserve guest memory", jit: false)
                return
            }
            var err = [CChar](repeating: 0, count: 256)
            let loaded = data.withUnsafeBytes { raw -> Int32 in
                let base = raw.bindMemory(to: UInt8.self).baseAddress
                return er_load_exe(m, base, data.count, &err, err.count)
            }
            guard loaded == 0 else {
                er_destroy(m)
                self.finish(status: "Load failed: \(String(cString: err))", jit: false)
                return
            }

            self.lock.lock(); self.machine = m; self.lock.unlock()
            let code = er_run(m, wantJIT ? 1 : 0)
            let jit = er_jit_active(m) != 0
            self.lock.lock(); self.machine = nil; self.lock.unlock()
            er_destroy(m)

            self.finish(status: "Exited with code \(code)", jit: jit)
        }
    }

    func stop() {
        lock.lock(); defer { lock.unlock() }
        if let m = machine { er_stop(m) }
    }

    private func finish(status: String, jit: Bool) {
        DispatchQueue.main.async {
            self.status = status
            self.jitWasActive = jit
            self.hasRun = true
            self.isRunning = false
        }
    }
}

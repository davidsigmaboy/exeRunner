import SwiftUI

struct ContentView: View {
    @StateObject private var emu = Emulator()
    @State private var showPicker = false

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                statusCard
                console
                controls
            }
            .padding()
            .navigationTitle("ExeRunner")
            .navigationBarTitleDisplayMode(.inline)
            .fileImporter(isPresented: $showPicker, allowedContentTypes: [.item]) { result in
                if case .success(let url) = result { emu.load(url: url) }
            }
        }
    }

    private var statusCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(emu.fileName ?? "No file selected").font(.headline)
            Text(emu.status).font(.subheadline).foregroundStyle(.secondary)
            HStack(spacing: 6) {
                Circle().fill(jitColor).frame(width: 8, height: 8)
                Text(jitText).font(.caption).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: 12))
    }

    private var jitColor: Color {
        if !emu.useJIT { return .gray }
        if !emu.jitAvailable { return .orange }
        if emu.hasRun && !emu.jitWasActive { return .orange }
        return .green
    }

    private var jitText: String {
        if !emu.useJIT { return "Interpreter only" }
        if !emu.jitAvailable { return "JIT not enabled for this process – attach a JIT enabler; using the interpreter" }
        if emu.hasRun { return emu.jitWasActive ? "JIT running" : "JIT available but not used (see console)" }
        return "JIT available"
    }

    private var console: some View {
        ScrollViewReader { proxy in
            ScrollView {
                Text(emu.output.isEmpty ? " " : emu.output)
                    .font(.system(.footnote, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(10)
                Color.clear.frame(height: 1).id("end")
            }
            .background(Color.black, in: RoundedRectangle(cornerRadius: 12))
            .onChange(of: emu.output) { _ in proxy.scrollTo("end") }
        }
    }

    private var controls: some View {
        VStack(spacing: 10) {
            Toggle("Use JIT", isOn: $emu.useJIT).disabled(emu.isRunning)
            HStack(spacing: 10) {
                Button { showPicker = true } label: {
                    Label("Open .exe", systemImage: "doc")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(emu.isRunning)

                if emu.isRunning {
                    Button(role: .destructive) { emu.stop() } label: {
                        Label("Stop", systemImage: "stop.fill").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                } else {
                    Button { emu.run() } label: {
                        Label("Run", systemImage: "play.fill").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(emu.fileName == nil)
                }
            }
        }
    }
}

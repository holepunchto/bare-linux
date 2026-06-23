// Phase 0 throwaway: echo every IPC message back unchanged. This isolates the
// native boundary (boot a worklet, exchange bytes over the IPC channel) from
// the real hrpc backend, which is wired in later.
//
// `Bare.IPC` is the duplex byte stream to the native host, injected by the
// worklet runtime. The real backend rides hrpc on top of it; here we just echo.
const { IPC } = Bare

IPC.on('data', (data) => IPC.write(data))

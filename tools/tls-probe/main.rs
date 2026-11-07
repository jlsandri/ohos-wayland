// Thread-locals plus a spawned thread: the case that breaks executables
// whose local-exec TLS is loaded with dlopen.
use std::cell::Cell;

thread_local! {
    static COUNTER: Cell<u32> = Cell::new(0);
}

fn main() {
    COUNTER.with(|c| c.set(41));
    let worker = std::thread::spawn(|| COUNTER.with(|c| c.get()));
    let other = worker.join().unwrap();
    COUNTER.with(|c| println!("TLS_PROBE main={} worker={}", c.get() + 1, other));
}

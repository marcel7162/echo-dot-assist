use crate::Error;
use byteorder::{BigEndian, ByteOrder};
use futures_core::ready;
use futures_util::{FutureExt, Sink, SinkExt, future};
use hmac::digest::Digest;
use sha1::Sha1;
use std::time::{Duration, Instant};
use std::{
    future::Future,
    mem,
    pin::Pin,
    task::{Context, Poll},
};
use tokio::{task::JoinHandle, time::timeout};

/// Returns a future that will flush the sink, even if flushing is temporarily completed.
/// Finishes only if the sink throws an error.
pub(crate) fn keep_flushing<'a, T, S: Sink<T> + Unpin + 'a>(
    mut s: S,
) -> impl Future<Output = S::Error> + 'a {
    future::poll_fn(move |cx| match s.poll_flush_unpin(cx) {
        Poll::Ready(Err(e)) => Poll::Ready(e),
        _ => Poll::Pending,
    })
}

pub struct CancelOnDrop<T>(pub JoinHandle<T>);

impl<T> Future for CancelOnDrop<T> {
    type Output = <JoinHandle<T> as Future>::Output;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.0.poll_unpin(cx)
    }
}

impl<T> Drop for CancelOnDrop<T> {
    fn drop(&mut self) {
        self.0.abort();
    }
}

pub struct TimeoutOnDrop<T: Send + 'static> {
    handle: Option<JoinHandle<T>>,
    timeout: tokio::time::Duration,
}

impl<T: Send + 'static> TimeoutOnDrop<T> {
    pub fn new(handle: JoinHandle<T>, timeout: tokio::time::Duration) -> Self {
        Self {
            handle: Some(handle),
            timeout,
        }
    }

    pub fn take(&mut self) -> Option<JoinHandle<T>> {
        self.handle.take()
    }
}

impl<T: Send + 'static> Future for TimeoutOnDrop<T> {
    type Output = <JoinHandle<T> as Future>::Output;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let r = ready!(
            self.handle
                .as_mut()
                .expect("Polled after ready")
                .poll_unpin(cx)
        );
        self.handle = None;
        Poll::Ready(r)
    }
}

impl<T: Send + 'static> Drop for TimeoutOnDrop<T> {
    fn drop(&mut self) {
        let mut handle = if let Some(handle) = self.handle.take() {
            handle
        } else {
            return;
        };

        if (&mut handle).now_or_never().is_some() {
            // Already finished
            return;
        }

        match tokio::runtime::Handle::try_current() {
            Ok(h) => {
                h.spawn(timeout(self.timeout, CancelOnDrop(handle)));
            }
            Err(_) => {
                // Not in tokio context, can't spawn
                handle.abort();
            }
        }
    }
}

pub trait Seq {
    fn next(&self) -> Self;
}

macro_rules! impl_seq {
    ($($ty:ty)*) => { $(
        impl Seq for $ty {
            fn next(&self) -> Self { (*self).wrapping_add(1) }
        }
    )* }
}

impl_seq!(u8 u16 u32 u64 usize);

#[derive(Debug, Clone, Copy, Hash, PartialEq, Eq, PartialOrd, Ord, Default)]
pub struct SeqGenerator<T: Seq>(T);

impl<T: Seq> SeqGenerator<T> {
    pub fn new(value: T) -> Self {
        SeqGenerator(value)
    }

    pub fn get(&mut self) -> T {
        let value = self.0.next();
        mem::replace(&mut self.0, value)
    }
}

pub fn solve_hash_cash(
    ctx: &[u8],
    prefix: &[u8],
    length: i32,
    dst: &mut [u8],
) -> Result<Duration, Error> {
    // after a certain number of seconds, the challenge expires
    const TIMEOUT: u64 = 5; // seconds
    let now = Instant::now();

    let md = Sha1::digest(ctx);

    let target: i64 = BigEndian::read_i64(&md[12..20]);

    // hassmic (README.md here): on every core, each worker taking every n-th counter.  Any counter whose hash has the
    // trailing zeros is an answer, so the first one any worker finds is taken.  One core of an Echo Dot 3 (Cortex-A35,
    // 32-bit, no SHA instructions used) took the 5 s for one challenge in three, and the speaker never logged in
    // (2026-11-19: "Unable to solve any of 3 hash cash challenges")
    let workers = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .clamp(1, 8) as i64;
    let found = std::sync::atomic::AtomicBool::new(false);
    let answer = std::sync::Mutex::new(None::<Vec<u8>>);
    std::thread::scope(|s| {
        for w in 0..workers {
            let (found, answer) = (&found, &answer);
            s.spawn(move || {
                let mut counter: i64 = w;
                let mut n: u32 = 0;
                loop {
                    n = n.wrapping_add(1);
                    if n % 1024 == 0
                        && (found.load(std::sync::atomic::Ordering::Relaxed)
                            || now.elapsed().as_secs() >= TIMEOUT)
                    {
                        return;
                    }
                    let suffix =
                        [target.wrapping_add(counter).to_be_bytes(), counter.to_be_bytes()].concat();
                    let mut hasher = Sha1::new();
                    hasher.update(prefix);
                    hasher.update(&suffix);
                    let md = hasher.finalize();
                    if BigEndian::read_i64(&md[12..20]).trailing_zeros() >= (length as u32) {
                        if !found.swap(true, std::sync::atomic::Ordering::Relaxed) {
                            if let Ok(mut a) = answer.lock() {
                                *a = Some(suffix);
                            }
                        }
                        return;
                    }
                    counter = counter.wrapping_add(workers);
                }
            });
        }
    });

    let Some(suffix) = answer.into_inner().ok().flatten() else {
        return Err(Error::deadline_exceeded(format!(
            "{TIMEOUT} seconds expired"
        )));
    };

    dst.copy_from_slice(&suffix);

    Ok(now.elapsed())
}

pub fn get_next_query_separator(url: &str) -> &'static str {
    match url.find('?') {
        Some(_) => "&",
        None => "?",
    }
}

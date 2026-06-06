<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { randomQuote } from '$lib/quotes';

  let phase = $state(0);
  let typedText = $state('');
  let showCursor = $state(true);

  // Live animated counters
  let tpsCount = $state(0);
  let latencyCount = $state(0);
  let teamsCount = $state(0);

  const quoteObj = randomQuote();
  const quote = quoteObj.text;
  const author = quoteObj.author;

  onMount(() => {
    setTimeout(() => phase = 1, 300);
    setTimeout(() => phase = 2, 1000);
    setTimeout(() => {
      phase = 3;
      typewrite(quote, 0);
    }, 1800);
    setTimeout(() => phase = 4, 1800 + quote.length * 40 + 400);
    setTimeout(() => phase = 5, 1800 + quote.length * 40 + 1200);

    const blink = setInterval(() => showCursor = !showCursor, 530);

    // Animate stat counters when visible
    setTimeout(() => {
      animateValue(v => tpsCount = v, 13990000, 80);
      animateValue(v => latencyCount = v, 810, 60);
      animateValue(v => teamsCount = v, 312, 50);
    }, 1800 + quote.length * 40 + 600);

    // Intersection observer for scroll animations
    const observer = new IntersectionObserver((entries) => {
      entries.forEach(e => {
        if (e.isIntersecting) {
          e.target.classList.add('visible');
        }
      });
    }, { threshold: 0.1 });

    setTimeout(() => {
      document.querySelectorAll('.reveal').forEach(el => observer.observe(el));
    }, 2000);

    return () => { clearInterval(blink); observer.disconnect(); };
  });

  function typewrite(text: string, i: number) {
    if (i <= text.length) {
      typedText = text.slice(0, i);
      setTimeout(() => typewrite(text, i + 1), 35 + Math.random() * 25);
    }
  }

  function animateValue(setter: (v: number) => void, target: number, frames: number) {
    let frame = 0;
    const step = () => {
      frame++;
      const p = 1 - Math.pow(1 - frame / frames, 3);
      setter(Math.round(target * p));
      if (frame < frames) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  }

  function fmtNum(n: number): string {
    if (n >= 1_000_000) return (n / 1_000_000).toFixed(2) + 'M';
    if (n >= 1_000) return (n / 1_000).toFixed(1) + 'K';
    return n.toString();
  }
</script>

<svelte:head>
  <title>IICPC — High-Frequency Trading Arena</title>
  <meta name="description" content="An institutional-grade benchmarking platform for competitive orderbook engineering. Submit your C++ engine, get scored on correctness, throughput, and latency." />
</svelte:head>

<!-- ============================================================ -->
<!-- HERO — FULL VIEWPORT CINEMATIC INTRO                         -->
<!-- ============================================================ -->
<div class="relative min-h-screen overflow-hidden bg-[var(--bg-void)]">
  <!-- Animated grid background -->
  <div class="absolute inset-0 transition-opacity duration-[2000ms] {phase >= 1 ? 'opacity-100' : 'opacity-0'}">
    <div class="absolute inset-0 bg-grid opacity-60"></div>
    <div class="absolute inset-0" style="background: radial-gradient(circle 600px at 50% 40%, rgba(99,91,255,0.07), transparent);"></div>
    <div class="absolute inset-x-0 h-px bg-gradient-to-r from-transparent via-[var(--accent)]/20 to-transparent"
         style="top: 40%; animation: float 6s ease-in-out infinite;"></div>
  </div>

  <!-- Orderbook Data Stream -->
  <div class="absolute inset-0 overflow-hidden pointer-events-none {phase >= 2 ? 'opacity-100' : 'opacity-0'} transition-opacity duration-[3000ms] flex justify-between px-10">
    <div class="flex flex-col gap-2 mt-20 opacity-30 flicker">
      {#each Array(20) as _, i}
        <div class="mono text-[10px] text-[var(--emerald)]" style="animation: slideDown {3 + Math.random() * 2}s linear infinite {Math.random()}s;">
          BID {(10000 + Math.random() * 500).toFixed(2)} | {(Math.random() * 100).toFixed(0)} @ {(Math.random() * 10).toFixed(4)}s
        </div>
      {/each}
    </div>
    <div class="flex flex-col gap-2 mt-40 opacity-30 flicker">
      {#each Array(20) as _, i}
        <div class="mono text-[10px] text-[var(--rose)]" style="animation: slideDown {3 + Math.random() * 2}s linear infinite {Math.random()}s;">
          ASK {(10500 + Math.random() * 500).toFixed(2)} | {(Math.random() * 100).toFixed(0)} @ {(Math.random() * 10).toFixed(4)}s
        </div>
      {/each}
    </div>
  </div>

  <!-- Content -->
  <div class="relative z-10 flex flex-col items-center justify-center min-h-screen px-6">
    <div class="text-center transition-all duration-[1200ms] {phase >= 2 ? 'opacity-100 translate-y-0' : 'opacity-0 translate-y-8'}">
      <h1 class="text-7xl md:text-9xl font-black tracking-tighter leading-none select-none display text-gradient glitch-hover">
        IICPC
      </h1>
      <div class="h-px w-24 mx-auto mt-4 bg-gradient-to-r from-transparent via-[var(--accent)] to-transparent
                  transition-all duration-1000 {phase >= 2 ? 'opacity-100 scale-x-100' : 'opacity-0 scale-x-0'}"></div>
    </div>

    <div class="mt-8 h-12 flex flex-col items-center justify-center">
      {#if phase >= 3}
        <p class="text-[var(--text-secondary)] text-sm md:text-base tracking-wide flex items-center">
          <span class="italic">"{typedText}"</span><span class="inline-block w-[6px] h-4 ml-1 bg-[var(--accent)] align-middle {showCursor ? 'opacity-100' : 'opacity-0'} transition-opacity"></span>
        </p>
      {/if}
      {#if phase >= 4}
        <p class="text-xs text-[var(--text-muted)] mono uppercase tracking-wider mt-2 fade-in">— {author}</p>
      {/if}
    </div>

    <div class="mt-12 flex flex-col items-center gap-4 transition-all duration-700 {phase >= 4 ? 'opacity-100 translate-y-0' : 'opacity-0 translate-y-6'}">
      <button class="btn btn-primary btn-pill text-base px-14 py-4 relative overflow-hidden group" onclick={() => goto('/auth')}>
        <span class="relative z-10">Enter the Arena</span>
        <div class="absolute inset-0 bg-gradient-to-r from-[var(--accent)] to-[#8b5cf6] opacity-0 group-hover:opacity-100 transition-opacity"></div>
      </button>
      <span class="text-xs text-[var(--text-ghost)] tracking-[0.2em] uppercase">Competition Platform</span>
    </div>
  </div>

  <!-- Scroll indicator -->
  <div class="absolute bottom-8 left-1/2 -translate-x-1/2 transition-all duration-700 {phase >= 5 ? 'opacity-100' : 'opacity-0'}">
    <div class="w-5 h-8 rounded-full border border-[var(--border-hover)] flex items-start justify-center p-1">
      <div class="w-1 h-2 rounded-full bg-[var(--text-muted)]" style="animation: float 2s ease-in-out infinite;"></div>
    </div>
  </div>
</div>

<!-- ============================================================ -->
<!-- LIVE STATS BAR                                                -->
<!-- ============================================================ -->
{#if phase >= 5}
<div class="border-t border-b border-[var(--border)] bg-[var(--bg-deep)] py-6 px-6 overflow-hidden">
  <div class="max-w-5xl mx-auto flex items-center justify-center gap-12 md:gap-20">
    <div class="text-center reveal">
      <div class="text-2xl md:text-3xl font-black mono text-[var(--text-primary)]">{fmtNum(tpsCount)}</div>
      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.15em] mt-1">TPS Peak</div>
    </div>
    <div class="w-px h-8 bg-[var(--border)]"></div>
    <div class="text-center reveal">
      <div class="text-2xl md:text-3xl font-black mono text-[var(--text-primary)]">{latencyCount}<span class="text-sm text-[var(--text-muted)] ml-1">ns</span></div>
      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.15em] mt-1">p99 Latency</div>
    </div>
    <div class="w-px h-8 bg-[var(--border)]"></div>
    <div class="text-center reveal">
      <div class="text-2xl md:text-3xl font-black mono text-emerald-400">{teamsCount}</div>
      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.15em] mt-1">Submissions</div>
    </div>
    <div class="w-px h-8 bg-[var(--border)]"></div>
    <div class="text-center reveal">
      <div class="text-2xl md:text-3xl font-black mono text-amber-400">0</div>
      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.15em] mt-1">Drops</div>
    </div>
  </div>
</div>

<!-- ============================================================ -->
<!-- HOW IT WORKS — CINEMATIC CARDS                               -->
<!-- ============================================================ -->
<section class="relative py-28 md:py-36 px-6 bg-cosmic overflow-hidden">
  <!-- Background effects -->
  <div class="absolute inset-0 bg-grid opacity-30 pointer-events-none"></div>
  <div class="absolute top-0 inset-x-0 h-px bg-gradient-to-r from-transparent via-[var(--accent)]/10 to-transparent"></div>

  <div class="max-w-5xl mx-auto relative z-10">
    <div class="text-center mb-20 reveal">
      <span class="badge badge-accent mb-4">The Process</span>
      <h2 class="text-4xl md:text-6xl font-black display tracking-tight text-gradient mt-4">
        Three steps. One benchmark.
      </h2>
      <p class="text-[var(--text-secondary)] mt-4 max-w-md mx-auto text-sm leading-relaxed">
        Upload your C++ orderbook engine. We compile, isolate, and stress-test it against deterministic order flow.
      </p>
    </div>

    <div class="grid md:grid-cols-3 gap-6 stagger">
      <!-- Step 1: Write -->
      <div class="card-glass p-8 group reveal">
        <div class="w-14 h-14 rounded-2xl flex items-center justify-center mb-6 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(99,91,255,0.15), rgba(99,91,255,0.05));">
          <svg class="w-6 h-6 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M17.25 6.75L22.5 12l-5.25 5.25m-10.5 0L1.5 12l5.25-5.25m7.5-3l-4.5 16.5"/>
          </svg>
        </div>
        <div class="text-xs text-[var(--accent)] mono font-bold mb-3 tracking-wider">01 — WRITE</div>
        <h3 class="text-xl font-bold display mb-3">Build Your Engine</h3>
        <p class="text-sm text-[var(--text-secondary)] leading-relaxed">
          Implement a matching engine in C++23. Price-time priority. No framework constraints — pure performance engineering.
        </p>
        <div class="mt-6 pt-4 border-t border-[var(--border)]">
          <div class="terminal !rounded-lg !border-0 !bg-[var(--bg-surface)]">
            <div class="terminal-body !max-h-none !p-3 text-[10px] leading-relaxed">
              <span class="text-[var(--violet)]">struct</span> <span class="text-[var(--cyan)]">Order</span> {'{'}<br/>
              &nbsp;&nbsp;<span class="text-[var(--violet)]">uint64_t</span> id;<br/>
              &nbsp;&nbsp;<span class="text-[var(--violet)]">Side</span> side;<br/>
              &nbsp;&nbsp;<span class="text-[var(--violet)]">Price</span> price;<br/>
              &nbsp;&nbsp;<span class="text-[var(--violet)]">Qty</span> qty;<br/>
              {'}'};
            </div>
          </div>
        </div>
      </div>

      <!-- Step 2: Submit -->
      <div class="card-glass p-8 group reveal">
        <div class="w-14 h-14 rounded-2xl flex items-center justify-center mb-6 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(6,182,212,0.15), rgba(6,182,212,0.05));">
          <svg class="w-6 h-6 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
          </svg>
        </div>
        <div class="text-xs text-[var(--cyan)] mono font-bold mb-3 tracking-wider">02 — SUBMIT</div>
        <h3 class="text-xl font-bold display mb-3">Upload & Isolate</h3>
        <p class="text-sm text-[var(--text-secondary)] leading-relaxed">
          We compile with <span class="mono text-[var(--text-primary)]">-O3 -march=native</span>. Your binary runs inside an isolated sandbox with pinned CPU cores and 2MB HugePages.
        </p>
        <div class="mt-6 pt-4 border-t border-[var(--border)]">
          <div class="space-y-2.5">
            {#each [['cgroups v2', 'Memory isolation'], ['seccomp', 'Syscall filtering'], ['CPU pinning', 'No core migration'], ['HugePages', 'Zero TLB misses']] as [label, desc]}
              <div class="flex items-center gap-2.5">
                <div class="w-1.5 h-1.5 rounded-full bg-[var(--cyan)]"></div>
                <span class="text-[11px] mono text-[var(--text-primary)]">{label}</span>
                <span class="text-[10px] text-[var(--text-ghost)]">— {desc}</span>
              </div>
            {/each}
          </div>
        </div>
      </div>

      <!-- Step 3: Compete -->
      <div class="card-glass p-8 group reveal">
        <div class="w-14 h-14 rounded-2xl flex items-center justify-center mb-6 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(16,185,129,0.15), rgba(16,185,129,0.05));">
          <svg class="w-6 h-6 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 18.75h-9m9 0a3 3 0 013 3h-15a3 3 0 013-3m9 0v-3.375c0-.621-.503-1.125-1.125-1.125h-.871M7.5 18.75v-3.375c0-.621.504-1.125 1.125-1.125h.872m5.007 0H9.497m5.007 0a7.454 7.454 0 01-.982-3.172M9.497 14.25a7.454 7.454 0 00.981-3.172"/>
          </svg>
        </div>
        <div class="text-xs text-[var(--emerald)] mono font-bold mb-3 tracking-wider">03 — COMPETE</div>
        <h3 class="text-xl font-bold display mb-3">Score & Rank</h3>
        <p class="text-sm text-[var(--text-secondary)] leading-relaxed">
          Stress-tested with millions of deterministic orders. Scored on correctness, throughput, and p99 latency. Real-time leaderboard.
        </p>
        <div class="mt-6 pt-4 border-t border-[var(--border)]">
          <div class="space-y-2">
            <div class="flex items-center gap-2">
              <div class="w-1.5 h-1.5 rounded-full bg-[var(--accent)]"></div>
              <span class="text-xs mono text-[var(--text-primary)]">Correctness Verification</span>
            </div>
            <div class="flex items-center gap-2">
              <div class="w-1.5 h-1.5 rounded-full bg-[var(--cyan)]"></div>
              <span class="text-xs mono text-[var(--text-primary)]">Throughput Measurement</span>
            </div>
            <div class="flex items-center gap-2">
              <div class="w-1.5 h-1.5 rounded-full bg-[var(--amber)]"></div>
              <span class="text-xs mono text-[var(--text-primary)]">Latency Profiling</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</section>

<!-- ============================================================ -->
<!-- ARCHITECTURE SHOWCASE                                        -->
<!-- ============================================================ -->
<section class="relative py-28 md:py-36 px-6 bg-[var(--bg-void)] overflow-hidden">
  <!-- Dramatic background -->
  <div class="absolute inset-0 pointer-events-none">
    <div class="absolute inset-0 bg-lines opacity-30"></div>
    <div class="absolute inset-0" style="background: radial-gradient(ellipse 60% 40% at 50% 50%, rgba(99,91,255,0.04), transparent);"></div>
  </div>

  <div class="max-w-5xl mx-auto relative z-10">
    <div class="text-center mb-20 reveal">
      <span class="badge badge-muted mb-4">Architecture</span>
      <h2 class="text-4xl md:text-6xl font-black display tracking-tight text-gradient mt-4">
        Built for nanoseconds
      </h2>
      <p class="text-[var(--text-secondary)] mt-4 max-w-lg mx-auto text-sm leading-relaxed">
        Every component eliminates jitter, avoids allocations, and maximizes cache locality.
      </p>
    </div>

    <div class="grid md:grid-cols-2 gap-5 stagger">
      <!-- Ring Buffers -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(99,91,255,0.15), rgba(99,91,255,0.04));">
          <svg class="w-5 h-5 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">Lock-Free Ring Buffers</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">SPSC queues with cache-line padding. Zero contention on the hot path.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--accent)]" style="text-shadow: 0 0 20px var(--accent-glow);">809M</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">ops/sec</div>
        </div>
      </div>

      <!-- Arena Allocator -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(6,182,212,0.15), rgba(6,182,212,0.04));">
          <svg class="w-5 h-5 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M21 7.5l-2.25-1.313M21 7.5v2.25m0-2.25l-2.25 1.313M3 7.5l2.25-1.313M3 7.5l2.25 1.313M3 7.5v2.25m9 3l2.25-1.313M12 12.75l-2.25-1.313M12 12.75V15m0 6.75l2.25-1.313M12 21.75V19.5m0 2.25l-2.25-1.313m0-16.875L12 2.25l2.25 1.313M21 14.25v2.25l-2.25 1.313m-13.5 0L3 16.5v-2.25"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">Arena Allocator</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Zero malloc on hot path. 2MB HugePage-backed bump allocator.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--cyan)]" style="text-shadow: 0 0 20px rgba(6,182,212,0.3);">0.3</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">ns/alloc</div>
        </div>
      </div>

      <!-- Ticket Spinlocks -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(16,185,129,0.15), rgba(16,185,129,0.04));">
          <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 10.5V6.75a4.5 4.5 0 10-9 0v3.75m-.75 11.25h10.5a2.25 2.25 0 002.25-2.25v-6.75a2.25 2.25 0 00-2.25-2.25H6.75a2.25 2.25 0 00-2.25 2.25v6.75a2.25 2.25 0 002.25 2.25z"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">Ticket Spinlocks</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Fair FIFO ordering. Minimal contention under load.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--emerald)]" style="text-shadow: 0 0 20px rgba(16,185,129,0.3);">8.6</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">ns/cycle</div>
        </div>
      </div>

      <!-- HDR Histograms -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(139,92,246,0.15), rgba(139,92,246,0.04));">
          <svg class="w-5 h-5 text-[var(--violet)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3 13.125C3 12.504 3.504 12 4.125 12h2.25c.621 0 1.125.504 1.125 1.125v6.75C7.5 20.496 6.996 21 6.375 21h-2.25A1.125 1.125 0 013 19.875v-6.75zM9.75 8.625c0-.621.504-1.125 1.125-1.125h2.25c.621 0 1.125.504 1.125 1.125v11.25c0 .621-.504 1.125-1.125 1.125h-2.25a1.125 1.125 0 01-1.125-1.125V8.625zM16.5 4.125c0-.621.504-1.125 1.125-1.125h2.25C20.496 3 21 3.504 21 4.125v15.75c0 .621-.504 1.125-1.125 1.125h-2.25a1.125 1.125 0 01-1.125-1.125V4.125z"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">HDR Histograms</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Zero-allocation latency tracking. Sub-nanosecond percentiles.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--violet)]" style="text-shadow: 0 0 20px rgba(139,92,246,0.3);">p99.9</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">tracked</div>
        </div>
      </div>

      <!-- io_uring -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(245,158,11,0.15), rgba(245,158,11,0.04));">
          <svg class="w-5 h-5 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M15.362 5.214A8.252 8.252 0 0112 21 8.25 8.25 0 016.038 7.048 8.287 8.287 0 009 9.6a8.983 8.983 0 013.361-6.867 8.21 8.21 0 003 2.48z"/>
            <path stroke-linecap="round" stroke-linejoin="round" d="M12 18a3.75 3.75 0 00.495-7.467 5.99 5.99 0 00-1.925 3.546 5.974 5.974 0 01-2.133-1A3.75 3.75 0 0012 18z"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">io_uring I/O</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Batched syscalls. Zero kernel transitions on hot path.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--amber)]" style="text-shadow: 0 0 20px rgba(245,158,11,0.3);">0</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">syscalls</div>
        </div>
      </div>

      <!-- QuestDB Telemetry -->
      <div class="card-glass p-7 flex items-start gap-5 group reveal">
        <div class="w-12 h-12 rounded-xl flex items-center justify-center flex-shrink-0 transition-transform group-hover:scale-110"
             style="background: linear-gradient(135deg, rgba(244,63,94,0.15), rgba(244,63,94,0.04));">
          <svg class="w-5 h-5 text-[var(--rose)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M7.5 14.25v2.25m3-4.5v4.5m3-6.75v6.75m3-9v9M6 20.25h12A2.25 2.25 0 0020.25 18V6A2.25 2.25 0 0018 3.75H6A2.25 2.25 0 003.75 6v12A2.25 2.25 0 006 20.25z"/>
          </svg>
        </div>
        <div class="flex-1 min-w-0">
          <h3 class="text-sm font-bold display mb-1">QuestDB Telemetry</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">ILP protocol streams metrics with zero serialization.</p>
        </div>
        <div class="text-right flex-shrink-0">
          <div class="text-xl font-black mono text-[var(--rose)]" style="text-shadow: 0 0 20px rgba(244,63,94,0.3);">1.4M</div>
          <div class="text-[9px] text-[var(--text-ghost)] mono uppercase">rows/sec</div>
        </div>
      </div>
    </div>
  </div>
</section>

<!-- ============================================================ -->
<!-- TECH STACK TICKER                                            -->
<!-- ============================================================ -->
<div class="border-t border-b border-[var(--border)] bg-[var(--bg-deep)] py-4 overflow-hidden">
  <div class="flex items-center gap-8 whitespace-nowrap" style="animation: ticker 30s linear infinite;">
    {#each ['C++23', 'io_uring', 'HugePages', 'Firecracker', 'gRPC', 'Protobuf', 'QuestDB', 'Redis', 'Redpanda', 'SvelteKit', 'FastAPI', 'Terraform', 'Docker', 'cgroups v2', 'seccomp', 'CPU Pinning', 'C++23', 'io_uring', 'HugePages', 'Firecracker', 'gRPC', 'Protobuf', 'QuestDB', 'Redis', 'Redpanda', 'SvelteKit', 'FastAPI', 'Terraform'] as tech}
      <span class="text-xs mono text-[var(--text-ghost)] tracking-wider">{tech}</span>
      <span class="text-[var(--border-hover)]">·</span>
    {/each}
  </div>
</div>

<!-- ============================================================ -->
<!-- CTA SECTION                                                  -->
<!-- ============================================================ -->
<section class="relative py-28 md:py-36 px-6 bg-cosmic overflow-hidden text-center">
  <div class="absolute inset-0 pointer-events-none">
    <div class="absolute w-[600px] h-[600px] rounded-full blur-[120px] opacity-[0.04] left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2"
         style="background: var(--accent);"></div>
  </div>

  <div class="relative z-10 max-w-xl mx-auto reveal">
    <h2 class="text-4xl md:text-5xl font-black display tracking-tight text-gradient mb-5">
      Ready to benchmark?
    </h2>
    <p class="text-[var(--text-secondary)] mb-10 text-sm leading-relaxed">
      Register your team, upload your orderbook implementation, and see how your engine stacks up against the competition.
    </p>
    <div class="flex justify-center gap-4">
      <button class="btn btn-primary btn-pill px-10 py-4 group" onclick={() => goto('/auth?tab=register')}>
        <span class="relative z-10">Get Started</span>
        <div class="absolute inset-0 bg-gradient-to-r from-[var(--accent)] to-[var(--violet)] opacity-0 group-hover:opacity-100 transition-opacity rounded-full"></div>
      </button>
      <button class="btn btn-ghost btn-pill px-10 py-4" onclick={() => goto('/auth')}>
        Sign In
      </button>
    </div>
  </div>
</section>

<!-- ============================================================ -->
<!-- FOOTER                                                       -->
<!-- ============================================================ -->
<footer class="border-t border-[var(--border)] py-8 px-6 bg-[var(--bg-void)]">
  <div class="max-w-5xl mx-auto">
    <div class="flex flex-col md:flex-row items-center justify-between gap-6">
      <div class="flex items-center gap-3">
        <span class="text-sm font-bold display">IICPC</span>
        <span class="text-[10px] text-[var(--text-ghost)]">·</span>
        <span class="text-[10px] text-[var(--text-ghost)] uppercase tracking-wider">High-Frequency Trading Arena</span>
      </div>
      <div class="flex items-center gap-6 text-[10px] text-[var(--text-ghost)]">
        <span class="flex items-center gap-1.5">
          <div class="w-1.5 h-1.5 rounded-full bg-emerald-500" style="animation: pulse-dot 2s ease infinite;"></div>
          All systems operational
        </span>
        <span class="mono">v1.0.0</span>
        <span>© 2026</span>
      </div>
    </div>
  </div>
</footer>
{/if}

<style>
  .reveal {
    opacity: 0;
    transform: translateY(24px);
    transition: opacity 0.8s cubic-bezier(0.16, 1, 0.3, 1), transform 0.8s cubic-bezier(0.16, 1, 0.3, 1);
  }
  .reveal.visible, .stagger > .reveal.visible {
    opacity: 1;
    transform: translateY(0);
  }
</style>

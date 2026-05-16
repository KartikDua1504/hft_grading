<script lang="ts">
  import '../app.css';
  let { children } = $props();
</script>

<svelte:head>
  <title>IICPC — High-Frequency Trading Arena</title>
  <meta name="description" content="An institutional-grade benchmarking platform for competitive orderbook engineering. Submit, test, and rank." />
  <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>⚡</text></svg>" />
</svelte:head>

{@render children()}

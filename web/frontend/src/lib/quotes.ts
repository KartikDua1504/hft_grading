// Pool of HFT / markets / competition quotes
const quotes = [
  { text: "The market is a device for transferring money from the impatient to the patient.", author: "Warren Buffett" },
  { text: "In trading, the impossible happens twice a year.", author: "Henri Poincaré" },
  { text: "Speed is the ultimate competitive moat.", author: "Anonymous Quant" },
  { text: "The best order is the one nobody else sees coming.", author: "HFT Proverb" },
  { text: "Latency is the tax you pay for indecision.", author: "Market Maker" },
  { text: "Every nanosecond of delay is a dollar someone else earned.", author: "Arb Trader" },
  { text: "Price discovery is a brutal meritocracy.", author: "Floor Trader" },
  { text: "The queue position is the most valuable real estate on earth.", author: "Orderbook Engineer" },
  { text: "Markets don't care about your architecture. They care about your throughput.", author: "Systems Architect" },
  { text: "The difference between first and second is measured in clock cycles.", author: "FPGA Developer" },
  { text: "An exchange doesn't sleep. Neither should your matching engine.", author: "C++ Optimization Lead" },
  { text: "Order flow is information. Speed is alpha.", author: "Prop Desk" },
  { text: "The spread is not a gap. It is an opportunity.", author: "Anonymous" },
  { text: "Simplicity at wire speed beats complexity at any speed.", author: "Network Engineer" },
  { text: "Your p99 latency is your actual latency. Everything else is marketing.", author: "Benchmarking Expert" },
  { text: "A dropped packet is a dropped opportunity.", author: "SysAdmin" },
  { text: "The best algorithms are invisible. The best fills are inevitable.", author: "Algo Developer" },
  { text: "Risk is not in the trade. Risk is in the infrastructure.", author: "Risk Manager" },
];

export function randomQuote(): { text: string, author: string } {
  return quotes[Math.floor(Math.random() * quotes.length)];
}

export function randomQuotes(n: number): { text: string, author: string }[] {
  const shuffled = [...quotes].sort(() => Math.random() - 0.5);
  return shuffled.slice(0, n);
}

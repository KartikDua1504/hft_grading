// Pool of HFT / markets / competition quotes
const quotes = [
  "The market is a device for transferring money from the impatient to the patient.",
  "In trading, the impossible happens twice a year.",
  "Speed is the ultimate competitive moat.",
  "The best order is the one nobody else sees coming.",
  "Latency is the tax you pay for indecision.",
  "Every nanosecond of delay is a dollar someone else earned.",
  "Price discovery is a brutal meritocracy.",
  "The queue position is the most valuable real estate on earth.",
  "Markets don't care about your architecture. They care about your throughput.",
  "The difference between first and second is measured in clock cycles.",
  "An exchange doesn't sleep. Neither should your matching engine.",
  "Order flow is information. Speed is alpha.",
  "The spread is not a gap. It is an opportunity.",
  "Simplicity at wire speed beats complexity at any speed.",
  "Your p99 latency is your actual latency. Everything else is marketing.",
  "A dropped packet is a dropped opportunity.",
  "The best algorithms are invisible. The best fills are inevitable.",
  "Risk is not in the trade. Risk is in the infrastructure.",
];

export function randomQuote(): string {
  return quotes[Math.floor(Math.random() * quotes.length)];
}

export function randomQuotes(n: number): string[] {
  const shuffled = [...quotes].sort(() => Math.random() - 0.5);
  return shuffled.slice(0, n);
}

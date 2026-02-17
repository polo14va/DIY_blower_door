const output = document.getElementById("output");

async function loadStatus() {
  output.textContent = "Loading...";
  try {
    const res = await fetch("/api/status");
    const data = await res.json();
    output.textContent = JSON.stringify(data, null, 2);
  } catch (err) {
    output.textContent = "Error calling /api/status";
  }
}
loadStatus();

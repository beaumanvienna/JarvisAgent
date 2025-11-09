const ws = new WebSocket("ws://localhost:8080/ws");

ws.onopen = () => {
  ws.send(JSON.stringify({
    type: "chat",
    subsystem: "engine",
    message: "Engine temperature warning light stays on"
  }));
};

ws.onmessage = (event) => {
  console.log("Received:", event.data);
};

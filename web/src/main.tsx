import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app/App";
import "./styles/index.css";
import { enablePerformanceRecording, reviewPerformance } from "./app/performance";
if (import.meta.env.DEV || new URLSearchParams(location.search).has("perf")) {
  enablePerformanceRecording();
  Object.assign(window, { aegisReviewPerformance: reviewPerformance });
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

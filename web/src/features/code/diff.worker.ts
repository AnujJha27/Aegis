import { diffHunks } from "./diff";
self.onmessage = (event: MessageEvent<{ original: string; modified: string }>) => {
  try { self.postMessage({ hunks: diffHunks(event.data.original, event.data.modified) }); }
  catch { self.postMessage({ error: "Could not prepare diff. Use the advanced viewer." }); }
};

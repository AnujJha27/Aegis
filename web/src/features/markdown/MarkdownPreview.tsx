import { useLayoutEffect, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { Marked } from "marked";
import { MermaidDiagram } from "./MermaidDiagram";
import { sanitizeMarkdown } from "./security";
const escape = (source: string) => source.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
export default function MarkdownPreview({ source }: { source: string }) {
  const root = useRef<HTMLDivElement>(null);
  const [targets, setTargets] = useState<Element[]>([]);
  const document = useMemo(() => {
    const diagrams: string[] = [];
    const parser = new Marked({ renderer: {
      html: ({ text }) => escape(text),
      code: ({ text, lang }) => {
        if (lang?.trim().toLowerCase() === "mermaid") {
          const index = diagrams.push(text) - 1;
          return `<div data-aegis-diagram="${index}"></div>`;
        }
        return `<pre><code>${escape(text)}</code></pre>`;
      },
    } });
    return { html: sanitizeMarkdown(parser.parse(source, { async: false })), diagrams };
  }, [source]);
  useLayoutEffect(() => { setTargets([...root.current?.querySelectorAll("[data-aegis-diagram]") ?? []]); }, [document]);
  return <div className="markdown-preview"><div ref={root} dangerouslySetInnerHTML={{ __html: document.html }} />{targets.map((target) => {
    const i = Number(target.getAttribute("data-aegis-diagram"));
    return document.diagrams[i] === undefined ? null : createPortal(<MermaidDiagram source={document.diagrams[i]} />, target, `${source.length}:${i}`);
  })}</div>;
}

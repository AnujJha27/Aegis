import DOMPurify from "dompurify";
export function sanitizeMarkdown(html: string): string {
  return DOMPurify.sanitize(html, { USE_PROFILES: { html: true }, FORBID_TAGS: ["img", "style", "form", "input", "button", "textarea", "iframe", "object", "embed"], FORBID_ATTR: ["style", "src", "srcset"] });
}
export function validateDiagramSource(source: string) {
  if (/url\s*\(|@import|!\[|\b(?:img|image|icon)\s*:/i.test(source)) throw new Error("External resources and image/icon directives are disabled.");
  if (source.length > 50000) throw new Error("Diagram exceeds the 50,000-character rendering limit.");
  // Configuration is application-owned. Preserve the original source in the UI, but never pass overrides to Mermaid.
  if (source.includes("%%{") || /^\s*---/.test(source)) throw new Error("Diagram configuration directives/frontmatter are disabled for repository Markdown.");
  if (/(?:^|;)\s*click\s/im.test(source)) throw new Error("Diagram links and callbacks are disabled.");
}
export function sanitizeSvg(svg: string): string {
  const safe = DOMPurify.sanitize(svg, { USE_PROFILES: { svg: true, svgFilters: true }, FORBID_TAGS: ["a", "script", "foreignObject", "image", "animate", "set", "animateTransform", "animateMotion"], FORBID_ATTR: ["target", "onload", "onclick"] });
  const document = new DOMParser().parseFromString(safe, "image/svg+xml");
  const root = document.documentElement;
  if (root.localName !== "svg" || document.querySelector("parsererror")) throw new Error("Diagram did not produce a valid SVG.");
  const unsafeCss = (value: string) => {
    const normalized = value.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\\([0-9a-f]{1,6})\s?/gi, (_, hex: string) => String.fromCodePoint(Math.min(parseInt(hex, 16), 0x10ffff))).replace(/\\([^\r\n])/g, "$1");
    return /@import|expression\s*\(|javascript:|url\s*\(\s*["']?\s*(?!#)[^\s]/i.test(normalized);
  };
  for (const node of root.querySelectorAll("*")) {
    if (node.localName === "style" && unsafeCss(node.textContent ?? "")) node.remove();
    for (const attribute of [...node.attributes]) {
      if (attribute.name.toLowerCase().startsWith("on") || /(?:^|:)href$/.test(attribute.name) && !attribute.value.startsWith("#") || unsafeCss(attribute.value)) node.removeAttribute(attribute.name);
    }
  }
  for (const attribute of [...root.attributes]) if (attribute.name.startsWith("on") || unsafeCss(attribute.value)) root.removeAttribute(attribute.name);
  return new XMLSerializer().serializeToString(root);
}
// A cached diagram may appear several times in one document. Namespace IDs per instance.
export function instanceSvg(svg: string, prefix: string): string {
  const document = new DOMParser().parseFromString(svg, "image/svg+xml");
  const ids = new Map<string, string>();
  for (const element of document.querySelectorAll("[id]")) ids.set(element.id, `${prefix}-${ids.size}`);
  for (const element of document.querySelectorAll("*")) {
    for (const attribute of [...element.attributes]) {
      if (attribute.name === "id") element.setAttribute("id", ids.get(attribute.value)!);
      else {
        let value = attribute.value;
        value = value.replace(/url\(#([^)]*)\)/g, (match, id: string) => ids.has(id) ? `url(#${ids.get(id)})` : match);
        if (value.startsWith("#") && ids.has(value.slice(1))) value = `#${ids.get(value.slice(1))}`;
        if (attribute.name === "aria-labelledby" || attribute.name === "aria-describedby") value = value.split(" ").map((id) => ids.get(id) ?? id).join(" ");
        element.setAttribute(attribute.name, value);
      }
    }
    if (element.localName === "style") {
      let css = element.textContent ?? "";
      css = css.replace(/#([\w-]+)/g, (match, id: string) => ids.has(id) ? `#${ids.get(id)}` : match);
      element.textContent = css;
    }
  }
  return new XMLSerializer().serializeToString(document.documentElement);
}

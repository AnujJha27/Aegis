import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import { loader } from "@monaco-editor/react";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";
import jsonWorker from "monaco-editor/esm/vs/language/json/json.worker?worker";
import cssWorker from "monaco-editor/esm/vs/language/css/css.worker?worker";
import htmlWorker from "monaco-editor/esm/vs/language/html/html.worker?worker";
import tsWorker from "monaco-editor/esm/vs/language/typescript/ts.worker?worker";
import "monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution";
import "monaco-editor/esm/vs/basic-languages/typescript/typescript.contribution";
import "monaco-editor/esm/vs/basic-languages/javascript/javascript.contribution";
import "monaco-editor/esm/vs/language/json/monaco.contribution";
import "monaco-editor/esm/vs/basic-languages/markdown/markdown.contribution";
import "monaco-editor/esm/vs/basic-languages/html/html.contribution";
import "monaco-editor/esm/vs/basic-languages/css/css.contribution";
import "monaco-editor/esm/vs/basic-languages/python/python.contribution";
import "monaco-editor/esm/vs/basic-languages/shell/shell.contribution";
import "monaco-editor/esm/vs/basic-languages/rust/rust.contribution";
import "monaco-editor/esm/vs/basic-languages/go/go.contribution";
import "monaco-editor/esm/vs/basic-languages/yaml/yaml.contribution";
import "monaco-editor/esm/vs/basic-languages/ini/ini.contribution";
import "monaco-editor/esm/vs/basic-languages/solidity/solidity.contribution";

(globalThis as typeof globalThis & { MonacoEnvironment: { getWorker: (_id: string, label: string) => Worker } }).MonacoEnvironment = {
  getWorker: (_id, label) => {
    if (label === "json") return new jsonWorker();
    if (["css", "scss", "less"].includes(label)) return new cssWorker();
    if (["html", "handlebars", "razor"].includes(label)) return new htmlWorker();
    if (["typescript", "javascript"].includes(label)) return new tsWorker();
    return new editorWorker();
  },
};

loader.config({ monaco });

monaco.editor.defineTheme("aegis-muted-dark", {
  base: "vs-dark",
  inherit: true,
  rules: [
    { token: "comment", foreground: "718087", fontStyle: "italic" },
    { token: "string", foreground: "b5b8a7" },
    { token: "keyword", foreground: "a6afb9" },
  ],
  colors: {
    "editor.background": "#151a21",
    "editor.foreground": "#c5cbd3",
    "editorLineNumber.foreground": "#626f7d",
    "editorLineNumber.activeForeground": "#aab6c4",
    "editorCursor.foreground": "#91a9c4",
    "editor.selectionBackground": "#293747",
    "editor.inactiveSelectionBackground": "#202a36",
    "editor.lineHighlightBackground": "#1a2028",
    "editorIndentGuide.background1": "#252d38",
    "editorWidget.background": "#171d26",
    "editorWidget.border": "#2a323d",
    "editorGutter.background": "#151a21",
    "diffEditor.insertedTextBackground": "#34483c88",
    "diffEditor.removedTextBackground": "#543b3888",
    "diffEditor.insertedLineBackground": "#26372f88",
    "diffEditor.removedLineBackground": "#38282688",
    "scrollbarSlider.background": "#626f7d44",
    "scrollbarSlider.hoverBackground": "#7f8b9966",
    "scrollbarSlider.activeBackground": "#91a9c477",
  },
});

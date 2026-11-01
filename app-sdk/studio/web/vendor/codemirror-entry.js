// LakeShark Studio's editor: CodeMirror 6, bundled once into one file (global CM).
export {
  EditorView, keymap, lineNumbers, highlightActiveLine, highlightActiveLineGutter, drawSelection, dropCursor,
  highlightSpecialChars, rectangularSelection, crosshairCursor, hoverTooltip, showTooltip, gutter, GutterMarker,
  scrollPastEnd,
} from '@codemirror/view';
export { EditorState, Compartment, StateEffect, StateField, Annotation, Transaction, Prec, RangeSet, RangeSetBuilder } from '@codemirror/state';
export { cpp, cppLanguage } from '@codemirror/lang-cpp';
export {
  autocompletion, completeFromList, closeBrackets, closeBracketsKeymap, completionKeymap, startCompletion,
} from '@codemirror/autocomplete';
export {
  HighlightStyle, syntaxHighlighting, defaultHighlightStyle, indentUnit, indentOnInput, bracketMatching, foldGutter,
  foldKeymap, syntaxTree,
} from '@codemirror/language';
export { setDiagnostics, lintGutter, lintKeymap, forEachDiagnostic, nextDiagnostic, openLintPanel } from '@codemirror/lint';
export {
  defaultKeymap, history, historyKeymap, indentWithTab, toggleComment, indentSelection, selectAll, undo, redo,
  insertNewlineAndIndent, insertNewlineKeepIndent, cursorDocStart,
} from '@codemirror/commands';
export {
  searchKeymap, highlightSelectionMatches, openSearchPanel, closeSearchPanel, gotoLine, selectNextOccurrence, search,
} from '@codemirror/search';
export { tags } from '@lezer/highlight';

// Copyright 2026 The Design++ Authors

export const hdlKeywords = [
  'always', 'always_comb', 'always_ff', 'always_latch', 'assign', 'automatic',
  'begin', 'bit', 'case', 'casex', 'casez', 'class', 'clocking', 'const',
  'constraint', 'default', 'do', 'else', 'end', 'endcase', 'endclass',
  'endfunction', 'endgenerate', 'endinterface', 'endmodule', 'endpackage',
  'endtask', 'enum', 'for', 'force', 'forever', 'fork', 'function', 'generate',
  'genvar', 'if', 'initial', 'inout', 'input', 'int', 'integer', 'interface',
  'join', 'join_any', 'join_none', 'localparam', 'logic', 'longint', 'module',
  'package', 'packed', 'parameter', 'program', 'ref', 'reg', 'release',
  'repeat', 'return', 'shortint', 'signed', 'static', 'string', 'struct',
  'task', 'time', 'typedef', 'union', 'unsigned', 'virtual', 'void', 'wait',
  'while', 'wire',
];

export function registerHdlLanguages(monaco) {
  for (const id of ['verilog', 'systemverilog']) {
    monaco.languages.register({id});
    monaco.languages.setLanguageConfiguration(id, {
      comments: {lineComment: '//', blockComment: ['/*', '*/']},
      brackets: [['{', '}'], ['[', ']'], ['(', ')']],
      autoClosingPairs: [
        {open: '{', close: '}'}, {open: '[', close: ']'},
        {open: '(', close: ')'}, {open: '"', close: '"'},
      ],
      surroundingPairs: [
        {open: '{', close: '}'}, {open: '[', close: ']'},
        {open: '(', close: ')'}, {open: '"', close: '"'},
      ],
      folding: {
        markers: {
          start: /^\s*(module|interface|package|class|function|task|begin)\b/,
          end: /^\s*(endmodule|endinterface|endpackage|endclass|endfunction|endtask|end)\b/,
        },
      },
    });
    monaco.languages.setMonarchTokensProvider(id, {
      keywords: hdlKeywords,
      tokenizer: {
        root: [
          [/`[a-zA-Z_$][\w$]*/, 'keyword.directive'],
          [/\$[a-zA-Z_$][\w$]*/, 'type.identifier'],
          [/[a-zA-Z_$][\w$]*/, {cases: {'@keywords': 'keyword', '@default': 'identifier'}}],
          [/\d+'[sS]?[bBoOdDhH][0-9a-fA-F_xXzZ?]+/, 'number'],
          [/\b\d+(\.\d+)?([eE][+-]?\d+)?\b/, 'number'],
          [/"([^"\\]|\\.)*$/, 'string.invalid'],
          [/"/, {token: 'string.quote', bracket: '@open', next: '@string'}],
          [/\/\*/, 'comment', '@comment'],
          [/\/\/.*$/, 'comment'],
          [/[{}()\[\]]/, '@brackets'],
          [/[;,.]/, 'delimiter'],
          [/[#@!~?:&|+\-*\/%^=<>]+/, 'operator'],
        ],
        comment: [
          [/[^/*]+/, 'comment'], [/\*\//, 'comment', '@pop'], [/[/*]/, 'comment'],
        ],
        string: [
          [/[^\\"]+/, 'string'], [/\\./, 'string.escape'],
          [/"/, {token: 'string.quote', bracket: '@close', next: '@pop'}],
        ],
      },
    });
  }
}

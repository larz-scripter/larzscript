" Vim syntax file for Larzscript (.lz)
" Language: Larzscript - the money-native, general-purpose language
if exists("b:current_syntax")
  finish
endif

syn keyword lzKeyword   let fn return if else while for in break continue try catch throw import as and or not
syn keyword lzMoney     price wallet pay from to require gas paywall subscribe has
syn keyword lzConstant  true false nil
syn keyword lzBuiltin   print len range str int float bool type abs min max sum sorted reversed floor ceil round sqrt pow chr ord assert input keys values push money map filter reduce join enumerate zip read_file write_file append_file file_exists exit args all any count unique hex bin oct gcd factorial sign clamp list dict env run capture cwd chdir listdir mkdir remove rename time clock sleep

syn match  lzComment   "#.*$"
syn match  lzNumber    "\<\d\+\(\.\d\+\)\?\>"
syn match  lzMoneyLit  "\$\d\+\(\.\d\+\)\?"
syn region lzString    start=/"/ skip=/\\./ end=/"/
syn region lzFString   start=/f"/ skip=/\\./ end=/"/ contains=lzInterp
syn region lzInterp    start=/{/ end=/}/ contained contains=ALLBUT,lzComment
syn match  lzFunction  "\<fn\s\+\zs\w\+"
syn match  lzOperator  "==\|!=\|<=\|>=\|\*\*\|//\|+=\|-=\|\*=\|/=\|%=\|[-+*/%<>=?:]"

hi def link lzKeyword   Keyword
hi def link lzMoney     Statement
hi def link lzConstant  Constant
hi def link lzBuiltin   Function
hi def link lzComment   Comment
hi def link lzNumber    Number
hi def link lzMoneyLit  Number
hi def link lzString    String
hi def link lzFString   String
hi def link lzFunction  Function
hi def link lzOperator  Operator

let b:current_syntax = "larzscript"

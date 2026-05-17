// 34-xml.s -- XML walking, transformation, and emission, no library
//
// In Python this would be `import xml.etree.ElementTree as ET`,
// `tree = ET.fromstring(text)`, then `.iter` / `.findall` /
// `.set`, and `ET.tostring` to serialise.  Several hundred KB of
// library.
//
// In Symta, an XML document IS a Symta nested list with the right
// shape:
//
//   <book>...</book>            [book ...]
//   <author>Nancy</author>      [author "Nancy"]
//   <tag>v</tag><tag>w</tag>    [tag "v"]  [tag "w"]
//
// So tree transformation is just `{}` term rewriting on the
// nested list -- no library, no `findall`, no XPath.  The bulk of
// real-world XML work is *transformation*, not parsing, and
// transformation is where Symta has a comparative advantage.
//
// (A tiny parser at the end of this file converts raw `<a><b/></a>`
//  text into the nested-list form, demonstrating that the input
//  side can be hand-rolled in 25 lines of `{}` and `.split`.)
//
// Run:  symta -f examples/34-xml.s


// ----------------------------------------------------------------
// 1. Our document, as a Symta nested list.
// ----------------------------------------------------------------
T: book
     title  , "Symta"
     author , "Nancy Sadkov"
     year   , "2026"
     tag    , "language"
     tag    , "lisp"
     tag    , "rewriting"

say "document:"
say T
say ""


// ----------------------------------------------------------------
// 2. Query: every <tag> child.
//    A child is a 2-element `[\name value]` list, so destructuring
//    in `.keep` picks them straight out.
// ----------------------------------------------------------------
Tags T(_@~){-[tag@]=} //take tail and remove non-tag pairs
say "tag children:"
for [_ V] Tags: say "  - [V]"
say ""

// Pull a specific child by name:
find_child Tree Name =
  Found Tree.tail.keep(?0 >< Name)
  if Found.n then Found.0.1 else No
say "title:  [find_child T title]"
say "year:   [find_child T year]"
say "author: [find_child T author]"
say ""


// ----------------------------------------------------------------
// 3. Transform: rename every <tag> to <keyword>.
//    REFAL pattern, two characters of body.
// ----------------------------------------------------------------
T2 [@T{[tag V] =: keyword V}]
say "after renaming tag -> keyword:"
say T2
say ""


// ----------------------------------------------------------------
// 4. Add a new child.  Symta lists are persistent: build a fresh
//    list with the new node spliced in.
// ----------------------------------------------------------------
T3 [@T2 [publisher "self"]]
say "after adding publisher:"
say T3
say ""


// ----------------------------------------------------------------
// 5. Re-emit as XML.  Short recursive serialiser; output is
//    well-formed XML.
// ----------------------------------------------------------------
xml_of V =
  if V.is_list and V.n >< 2 and V.1.is_text
    then '<' + V.0 + '>' + V.1 + '</' + V.0 + '>'
    else if V.is_list
      then '<' + V.0 + '>' + V.tail{&xml_of}.text('') + '</' + V.0 + '>'
      else "[V]"

say "--- re-emitted XML ---"
say: xml_of T3
say ""


// ----------------------------------------------------------------
// 6. Deep recursive transform: collect every text-leaf inside the
//    tree.  This is the kind of walk that's a five-line recursive
//    function in any language, but Symta's destructuring makes it
//    feel like one line of intent.
// ----------------------------------------------------------------
all_text V =
  if V.is_text then [V]
  else if V.is_list then V.tail{&all_text}.j
  else []

Texts all_text T3
say "every text in T3:"
for S Texts: say "  - [S]"

Lists:
- Scroll with "scroll X Y DY" (X, Y on the list), never swipe: a swipe does not move a list.
- One scroll per reply, as the last DO line: look at where it landed before scrolling again or
  tapping (later DO lines after a scroll are dropped).
- Scroll less than one screen at a time (about 400) so nothing is skipped, and read every visible
  item. Note the section headers you pass: if the item's section is behind you, scroll back less.
- Open a row with "tap #N" (its tag), not with coordinates. A row cut off at the bottom edge or
  behind a bar or the search field cannot be tapped: scroll it fully into view first.
- If you reached both ends of the list and the item is not there, fail and name the closest items
  you saw; do not keep scrolling.

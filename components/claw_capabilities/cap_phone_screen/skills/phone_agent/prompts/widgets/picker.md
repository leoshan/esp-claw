Pickers (time, date, number wheels):
- Time picker: tap the hour number in the selected (center) row, then type the WHOLE time as four
  digits HHMM in one type action, as two DO lines of one reply (e.g. 7:00 AM -> DO: tap X Y /
  DO: type 0700; 6:30 -> type 0630). iOS fills the typed digits into the whole time from the right,
  NOT into one column: typing only "07" gives 0:07 (shown as 12:07 凌晨), so never type just two
  digits. If the result is wrong, tap the hour again and retype all four digits.
- Typing keeps the current half of the day: typed digits set only the 12-hour clock time (the hour
  wheel runs 0-11), and 24-hour input such as 2000 is rejected. So 0800 typed while the picker shows
  晚上 gives 晚上 8:00 (8 PM).
- AM/PM: the left word column (凌晨/早上/上午/中午/下午/晚上) moves the hour with it, so tapping a word
  just turns the wheel and lands on an unpredictable time. Never tap these words. If the selected
  word is in the wrong half of the day (AM = 凌晨/早上/上午, PM = 下午/晚上), FIRST wheel the word
  column 3 rows at a time towards the wanted half (ROWS < 0 towards 凌晨, ROWS > 0 towards 晚上)
  until any word of that half is selected; the hour it lands on does not matter. THEN tap the hour
  and type the whole HHMM.
- Check a time together with its word: 8:00 早上 or 8:00 上午 is 8:00 AM; 8:00 晚上 is 8:00 PM;
  12:07 凌晨 is not 7:00 AM.
- Only when typing did not change a wheel (or for word wheels): wheel X Y ROWS ROW_H on the column's
  selected row: ROWS > 0 moves to bigger values / later items, ROWS < 0 to smaller / earlier; ROW_H
  is the pixel distance between two neighbouring rows of that column. At most 3 rows per wheel
  action, one column per reply, then check.
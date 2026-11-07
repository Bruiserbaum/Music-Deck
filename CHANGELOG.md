# Changelog

## 0.2.1
- The row you tap stays lit as the one you picked, until you pick another, instead of a blink.
- A tap that could not be sent says "Did not start" instead of nothing.
- The tile can take the whole page as soon as Tessera's Layout offers that to plugins.

## 0.2.0
- A new look. On a wide screen (10 inches) three columns: your speakers down the left, what plays in the middle, your
  library at the right. On a smaller one the speakers stay at the left and a key switches between what plays and the
  library.
- What plays shows its cover, title, artist and how far it is (a bar with the times), with larger keys centred at the
  bottom: previous, stop, play or pause, next.
- Volume for the chosen speaker: tap the bar where you want it, or the speaker key to mute.
- Each speaker has a toggle: tap it to let that speaker play along with the chosen one, again to stop it. Move here
  takes the music from a speaker that plays it elsewhere to the chosen one.
- Up to six speakers.
- A page of the library that got lost on its way is asked for again by itself before the deck says there was no answer.
- A row that does not start says so.

## 0.1.3
- A tap on a row without a speaker says where to choose one, instead of nothing.
- Every media player can be the tile's speaker, also one without Music Assistant's mark.

## 0.1.2
- The list shows: Music Assistant answers with its rows under `items`, which the deck now reads.

## 0.1.1
- When the list does not come, the deck says why: Home Assistant's own words, no answer, or still waiting for
  Tessera just after the screen starts (asked again within seconds).

## 0.1.0
- First version: your Music Assistant playlists, albums, artists or radio on a tile, with filters you pick, up to four
  speakers, what plays now with play, pause and skip, and a tap on a row that plays it.

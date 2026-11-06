# Music deck

> Needs [Music Assistant](https://www.music-assistant.io) in Home Assistant, with your music services added there
> (YouTube Music, Spotify, and so on). Have its ID at hand before you add the plugin (step 1 below).

Your Music Assistant library on the screen, with the speakers you pick and what plays now.

- **Speakers** down the left. Tap one to play there. The toggle at its right lets it play along with the chosen
  speaker (tap again to stop it). When the chosen speaker is quiet and another plays, **Move here** brings the music
  over.
- **What plays**: its cover, title and artist, a bar with how far it is, and large keys at the bottom: previous, stop,
  play or pause, next. Under them the chosen speaker's volume: tap the bar where you want it, the speaker key mutes.
- **Your library**: the filters you picked (All, Favorites, Recently played, Most played, Newest, Shuffled) and a page
  of your playlists. Tap a playlist and it plays on the chosen speaker.

Instead of playlists the list can hold albums, artists, radio stations, podcasts or audiobooks, and it can keep only the
names with a word you choose ("Metal", "Chill").

Every account stays in Music Assistant. The screen never signs in anywhere: it asks Music Assistant for a page of the
library and tells it what to play.

## Set up

1. Find your Music Assistant ID: in Home Assistant, open **Settings > Devices & services > Music Assistant**, open the
   entry's **⋮** menu and choose **Copy ID**. (It is also the last part of that page's address.)
2. Add the plugin to a screen in Tessera and paste the ID into **Music Assistant ID**. It is the same for every
   screen. The screen builds once.
3. In Layout, place the **Music deck** tile from the library's Plugins group, and choose its player in the inspector
   (a Music Assistant player, the one you use most).
4. Make the tile as large as the page, so the deck fills the screen: all five columns and five rows on a 10-inch
   screen, two by three on a 4-inch one.
5. Optional, in the inspector:
   - **Second to Sixth speaker**: more players for the column at the left.
   - **Show**: playlists, albums, artists, radio stations, podcasts or audiobooks.
   - **Filters on the screen**: the chips above the list, in the order you put them. None chosen gives All,
     Favorites and Recent.
   - **Only names with**: a word to keep only those names.
   - **A tap on a row**: play now in place of the queue (the default), play now and keep the queue, play next, or add
     to the end.

## Sizes

- **Wide** (a 10-inch screen, about four columns and more): three columns, the speakers, what plays and the library
  side by side.
- **Smaller** (a 4-inch screen, or two or three columns): the speakers at the left, and at the right what plays or the
  library. The key at the right of the playing keys switches between the two; tapping a playlist turns to what plays.
- **Small tiles**: what plays and a play key. Tap the key to play or pause; tap anywhere else to open the whole deck as
  a card over the page.
- When the chips do not fit side by side, one chip shows the current choice and each tap steps to the next.

## Good to know

- The list comes from Music Assistant's `music_assistant.get_library` action, up to twelve names a page.
- Music Assistant keeps one library for all your services, so the list shows what is in your library, whatever service
  it came from. Music Assistant does not say which service an item came from, so there is no filter per service yet.
- The cover shows for the tile's own player, and for another speaker while it plays along with that one. A speaker
  that plays on its own shows its title and artist without a cover (Tessera shows a tile the pictures of its own
  player only).
- Playing along needs speakers Music Assistant can group: players of the same kind (Music Assistant's own sync
  groups). A toggle Home Assistant refuses goes back after a few seconds.
- Use the Music Assistant player of each speaker, not the one of its own integration (Cast, Sonos, and so on): only
  Music Assistant's players play your library.
- A board without the memory for pictures (the CYD, the Waveshare 3.5-inch, the Hosyond 4-inch) shows a music icon
  where the cover would be.
- The screen must be allowed to perform Home Assistant actions (the ESPHome integration's option, which Tessera turns
  on when it adds the screen).

## Privacy

Nothing leaves your home through this plugin. The screen asks Tessera, Tessera asks Music Assistant in your Home
Assistant, and a tap tells Music Assistant what to play. Music Assistant reaches your music services with its own
accounts, as it does without this plugin.

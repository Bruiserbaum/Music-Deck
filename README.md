# Music deck

> Needs [Music Assistant](https://www.music-assistant.io) in Home Assistant, with your music services added there
> (YouTube Music, Spotify, and so on). Have its ID at hand before you add the plugin (step 1 below).

Your Music Assistant library on the screen. The tile shows what plays now, with its cover, play or pause, and skip.
Under it are the speakers you picked, the filters you picked (All, Favorites, Recently played, Most played, Newest,
Shuffled), and a page of your playlists. Tap a playlist and it plays on the speaker that is lit.

Instead of playlists the list can hold albums, artists, radio stations, podcasts or audiobooks, and it can keep only the
names with a word you choose ("Metal", "Chill").

Every account stays in Music Assistant. The screen never signs in anywhere: it asks Music Assistant for a page of the
library and tells it what to play.

## Set up

1. Find your Music Assistant ID: in Home Assistant, open **Settings > Devices & services > Music Assistant**, open the
   entry's **⋮** menu and choose **Copy ID**. (It is also the last part of that page's address.)
2. Add the plugin to a screen in Tessera and paste the ID into **Music Assistant ID**. It is the same for every
   screen. The screen builds once.
3. In Layout, place the **Music deck** tile from the library's Plugins group, and choose its player in the inspector:
   it offers the players of Music Assistant only.
4. Optional, in the inspector:
   - **Show**: playlists, albums, artists, radio stations, podcasts or audiobooks.
   - **Filters on the screen**: the chips above the list, in the order you put them. None chosen gives All,
     Favorites and Recent.
   - **Only names with**: a word to keep only those names.
   - **Second, Third and Fourth speaker**: more players to choose from on the tile, as chips beside its own.
   - **A tap on a row**: play now in place of the queue (the default), play now and keep the queue, play next, or add
     to the end.

## Sizes

- **Large enough for a list** (about two columns and three rows on a 4-inch screen, less on a 10-inch one): the whole
  deck on the tile.
- **Smaller**: what plays now and a play key. Tap the key to play or pause; tap anywhere else to open the whole deck
  as a card over the page.
- The card and the tile work the same way: tap a speaker chip to choose where it plays, a filter chip to change the
  list, the arrows to page through it.
- When the chips do not fit side by side, one chip shows the current choice and each tap steps to the next.

## Good to know

- The list comes from Music Assistant's `music_assistant.get_library` action, up to twelve names a page.
- Music Assistant keeps one library for all your services, so the list shows what is in your library, whatever service
  it came from. Music Assistant does not say which service an item came from, so there is no filter per service yet.
- The cover shows for the tile's own player. A speaker added as Second, Third or Fourth speaker shows its title and
  artist without a cover.
- A board without the memory for pictures (the CYD, the Waveshare 3.5-inch, the Hosyond 4-inch) shows a music icon
  where the cover would be.
- The screen must be allowed to perform Home Assistant actions (the ESPHome integration's option, which Tessera turns
  on when it adds the screen).

## Privacy

Nothing leaves your home through this plugin. The screen asks Tessera, Tessera asks Music Assistant in your Home
Assistant, and a tap tells Music Assistant what to play. Music Assistant reaches your music services with its own
accounts, as it does without this plugin.

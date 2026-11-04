#pragma once
#include <cstdio>
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::music_deck {

// The tile's own player and up to three more (the options speaker_2 to speaker_4).
static constexpr int SPEAKERS = 4;
// The filters the manifest offers, in its order.
static constexpr int FILTERS = 6;
// Rows of one page at most: twelve items of Music Assistant's answer stay well under the 3.2 KB the app hands on whole
// (one with its picture's address cut to 48 bytes is about 210).
static constexpr int ROWS = 12;

// What a deck shows, from the tile's options. Fixed fields: one block in PSRAM (docs/FIRMWARE_API.md, "Memory").
struct Setup {
  char kind[16] = "playlist";
  char contains[68] = {};
  char enqueue[16] = "replace";
  uint8_t filters[FILTERS] = {};   // indexes into the deck's FILTER table
  uint8_t filter_count = 0;
  char extra[SPEAKERS - 1][72] = {};   // the entity ids of speaker_2 to speaker_4, "" for none
};

// One speaker the deck plays on, as Home Assistant last said it.
struct Speaker {
  char entity[72] = {};
  char name[52] = {};
  char state[16] = {};
  char title[52] = {};
  char artist[52] = {};
  int8_t option = -1;   // which of speaker_2 to speaker_4 it is (0 to 2), -1 for the tile's own player
};

// What a tile hands the card it opens: the card is made after the tap and knows only the entity and the tile.
struct Handoff : public tessera::InPsram {
  Setup setup;
  Speaker speakers[SPEAKERS];
  int count = 0;
};

class Deck;

// The plugin: the tile "deck" and the card "deck" a tap on a small tile opens. It keeps the Music Assistant entry to ask
// (the input `entry`) and hands each answer of the app to the decks on the glass.
class MusicDeck : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(JsonObjectConst message) override;

  void set_entry(const char *entry) { snprintf(entry_, sizeof(entry_), "%s", entry); }
  const char *entry() const { return entry_; }

  void join(Deck *deck);
  void leave(Deck *deck);
  Handoff &handoff();

 private:
  char entry_[72] = {};
  Deck *decks_[8] = {};
  Handoff *handoff_ = nullptr;
};

}  // namespace esphome::music_deck

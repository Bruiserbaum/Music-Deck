#include "music_deck.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include "esphome/core/hal.h"

namespace esphome::music_deck {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// Music Assistant's own actions: a page of the library (asked through the app, permissions.ha_commands), a row that
// plays, and a queue that moves from one speaker to another.
static const char *const LIBRARY = "call_service:music_assistant.get_library";
static const char *const PLAY = "music_assistant.play_media";
static const char *const TRANSFER = "music_assistant.transfer_queue";
// Home Assistant's own actions on the chosen speaker. Play and pause go through wish(), mute too.
static const char *const NEXT = "media_player.media_next_track";
static const char *const PREVIOUS = "media_player.media_previous_track";
static const char *const STOP = "media_player.media_stop";
static const char *const VOLUME = "media_player.volume_set";
static const char *const JOIN = "media_player.join";
static const char *const UNJOIN = "media_player.unjoin";

static const uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_STOP = 0xF04DB, ICON_NEXT = 0xF04AD,
                      ICON_PREVIOUS = 0xF04AE, ICON_LEFT = 0xF0141, ICON_RIGHT = 0xF0142, ICON_MUSIC = 0xF075A,
                      ICON_LIST = 0xF0CB8, ICON_VOLUME = 0xF057E, ICON_MUTED = 0xF075F, ICON_SPEAKER = 0xF04C3,
                      ICON_SPEAKER_OFF = 0xF04C4, ICON_JOINED = 0xF05E0, ICON_ADD = 0xF0415;

// The option ids of the extra speakers, in the order of Setup::extra.
static const char *const SPEAKER_OPTIONS[SPEAKERS - 1] = {"speaker_2", "speaker_3", "speaker_4", "speaker_5",
                                                          "speaker_6"};

// The filters of the manifest's `filters`, in its order: the value the option keeps, the word on its chip, and how
// Music Assistant sorts and keeps the library for it (music_assistant.get_library's order_by and favorite).
struct Filter {
  const char *value, *word, *order;
  bool favorite;
};
static const Filter FILTER[FILTERS] = {
    {"all", "all", "sort_name", false},
    {"favorites", "favorites", "sort_name", true},
    {"recent", "recent", "last_played_desc", false},
    {"most", "most", "play_count_desc", false},
    {"added", "added", "timestamp_added_desc", false},
    {"random", "random", "random", false},
};

static int filter_of(const char *value) {
  for (int i = 0; i < FILTERS; ++i)
    if (!strcmp(FILTER[i].value, value)) return i;
  return -1;
}

// Where an older Music Assistant files a kind: {"playlists": [...], "albums": [...], "radio": [...]}.
static std::string shelf(const char *kind) { return !strcmp(kind, "radio") ? "radio" : std::string(kind) + "s"; }

// The app cuts a text at 48 bytes without a mark: an address that long may have lost its end.
static bool maybe_cut(const char *text) { return strlen(text) >= 47; }

// A copy into a fixed field, cut where it ends.
template <size_t N> static void keep(char (&field)[N], const char *text) { snprintf(field, N, "%s", text ? text : ""); }

// 3:07, or 1:02:41 from an hour on.
static std::string clock_of(int32_t s) {
  char b[16];
  if (s < 0) s = 0;
  if (s >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", (int) (s / 3600), (int) (s / 60 % 60), (int) (s % 60));
  else snprintf(b, sizeof(b), "%d:%02d", (int) (s / 60), (int) (s % 60));
  return b;
}

static Setup setup_of(JsonObjectConst o) {
  Setup s;
  keep(s.kind, o["kind"] | "playlist");
  if (!s.kind[0]) keep(s.kind, "playlist");
  keep(s.contains, o["contains"] | "");
  keep(s.enqueue, o["enqueue"] | "replace");
  if (!s.enqueue[0]) keep(s.enqueue, "replace");
  // A choice with `multiple` keeps picks ({value, label}); a plain text is taken as a value too.
  for (JsonVariantConst pick : o["filters"].as<JsonArrayConst>()) {
    const char *value = pick.is<const char *>() ? pick.as<const char *>() : (pick["value"] | "");
    const int f = filter_of(value);
    if (f < 0 || s.filter_count >= FILTERS) continue;
    bool twice = false;
    for (int i = 0; i < s.filter_count; ++i) twice = twice || s.filters[i] == f;
    if (!twice) s.filters[s.filter_count++] = f;
  }
  if (!s.filter_count) {
    s.filters[0] = 0;   // All
    s.filters[1] = 1;   // Favorites
    s.filters[2] = 2;   // Recent
    s.filter_count = 3;
  }
  for (int i = 0; i < SPEAKERS - 1; ++i) keep(s.extra[i], o[SPEAKER_OPTIONS[i]] | "");
  return s;
}

static void shown(lv_obj_t *o, bool on) {
  if (!o) return;
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// A part that takes a tap: a block and the label in it, and where it lies.
struct Part {
  lv_obj_t *box = nullptr, *label = nullptr;
  int x = 0, y = 0, w = 0, h = 0;
  bool shown = false;

  bool hit(tessera::Point p, int slack) const {
    return shown && p.x >= x - slack && p.x < x + w + slack && p.y >= y - slack && p.y < y + h + slack;
  }
  void place(int px, int py, int pw, int ph) {
    x = px, y = py, w = pw, h = ph;
    lv_obj_set_size(box, pw, ph);
    lv_obj_set_pos(box, px, py);
  }
  void show(bool on) {
    shown = on && box;
    ::esphome::music_deck::shown(box, on);
  }
};

static lv_obj_t *still_block(lv_obj_t *parent, theme::Role fill) {
  lv_obj_t *o = ui::block(parent, fill);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

// A thin rounded bar (the progress and the volume): its track or its lit part.
static lv_obj_t *bar_block(lv_obj_t *parent, theme::Role fill, int x, int y, int w, int h) {
  lv_obj_t *o = still_block(parent, fill);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, h / 2, 0);
  return o;
}

// A round key with an icon in its middle.
static void make_key(Part &k, lv_obj_t *parent, int size, uint32_t icon, Font font = Font::ICON_SMALL) {
  k.box = still_block(parent, theme::RAISED);
  lv_obj_set_style_radius(k.box, LV_RADIUS_CIRCLE, 0);
  k.label = ui::label(k.box, font, theme::INK);
  ui::set_text(k.label, ui::icon(icon));
  k.place(0, 0, size, size);
  lv_obj_center(k.label);
  k.show(true);
}

// A chip: a pill with a word in it, laid out later as its word decides.
static void make_chip(Part &c, lv_obj_t *parent) {
  c.box = still_block(parent, theme::RAISED);
  c.label = ui::label(c.box, Font::BODY, theme::INK);
  lv_obj_set_style_text_align(c.label, LV_TEXT_ALIGN_CENTER, 0);
  c.show(false);
}

// The deck. Three ways to lie on the glass:
// - THREE columns where it is wide (a 10-inch screen): the speakers down the left, each with its toggle; what plays in
//   the middle (the cover, the words, the progress, then the keys and the volume at the bottom); the library at the
//   right (the filters, the page keys and a page of rows).
// - STACKED elsewhere (a 4-inch screen, a card on a small one): the speakers down the left where they fit, else as one
//   row of chips; at the right either what plays or the library, a key beside the playing keys switches; the keys and
//   the volume stay at the bottom.
// - SMALL, a tile too small for that: what plays and a play key; a tap beside the key opens the whole deck as a card.
// The tile and the card each hold one; the plugin hands each the answers of the app.
class Deck : public tessera::InPsram {
 public:
  Deck(MusicDeck *plugin, const Setup &setup, const char *entity) : plugin_(plugin), setup_(setup) {
    if (entity && *entity) keep(speakers_[count_++].entity, entity);
    for (int i = 0; i < SPEAKERS - 1 && count_ < SPEAKERS; ++i) {
      if (!setup_.extra[i][0] || (entity && !strcmp(setup_.extra[i], entity))) continue;
      bool twice = false;
      for (int k = 0; k < count_; ++k) twice = twice || !strcmp(speakers_[k].entity, setup_.extra[i]);
      if (twice) continue;
      keep(speakers_[count_].entity, setup_.extra[i]);
      speakers_[count_++].option = static_cast<int8_t>(i);
    }
    plugin_->join(this);
  }
  ~Deck() { plugin_->leave(this); }

  // The speakers as a tile last heard them, for a card that opens before Home Assistant says them again.
  void know(const Handoff &h) {
    for (int i = 0; i < h.count && i < count_; ++i)
      if (!strcmp(h.speakers[i].entity, speakers_[i].entity)) speakers_[i] = h.speakers[i];
    if (h.at >= 0 && h.at < count_) at_ = h.at;
    stated_ = h.count > 0;
    view_list_ = !(count_ && active(speakers_[at_]));
  }
  void tell(Handoff &h) const {
    h.setup = setup_;
    h.count = count_;
    h.at = at_;
    for (int i = 0; i < count_; ++i) h.speakers[i] = speakers_[i];
  }
  const char *own() const { return count_ && speakers_[0].option < 0 ? speakers_[0].entity : ""; }

  // Lays the deck out in `parent`. `whole`: the card, which is always the whole deck; a tile is when it has the room.
  void make(lv_obj_t *parent, int w, int h, bool whole) {
    w_ = w;
    h_ = h;
    pictures_ = ui::pictures();
    // Keys, chips and rows stand out from what lies under them: the page's ground under a card, the card under a tile.
    surface_ = whole ? theme::RAISED : theme::TRACK;
    gap_ = ui::px(8);
    key_ = std::max(ui::mm(8), ui::line_height(Font::ICON_SMALL) + ui::px(14));
    row_h_ = std::max(ui::mm(8), ui::line_height(Font::BODY_LARGE) + ui::px(12));
    row_gap_ = ui::px(4);
    full_ = whole || (w >= ui::mm(50) && h >= ui::mm(38));
    if (!full_) {
      make_small(parent);
      paint();
      return;
    }
    three_ = w >= ui::mm(150) && h >= ui::mm(50);
    if (three_) make_three(parent);
    else make_stack(parent);
    lay_chips();
    ask(true);
    paint();
  }

  void state(JsonObjectConst data) {
    if (!data["state"].isNull() && count_ && speakers_[0].option < 0) take(speakers_[0], data);
    JsonObjectConst entities = data["entities"].as<JsonObjectConst>();
    if (!entities.isNull())
      for (int i = 0; i < count_; ++i)
        if (speakers_[i].option >= 0 && !entities[SPEAKER_OPTIONS[speakers_[i].option]].isNull())
          take(speakers_[i], entities[SPEAKER_OPTIONS[speakers_[i].option]].as<JsonObjectConst>());
    settle();
    // The first word of Home Assistant decides what a stacked deck shows first: what plays, or else the library.
    if (!stated_) {
      stated_ = true;
      view_list_ = !(count_ && active(speakers_[at_]));
    }
    lay_chips();
    paint();
  }

  void tick(uint32_t epoch) {
    epoch_ = epoch;
    const uint32_t now = esphome::millis();
    if (hint_at_ && now - hint_at_ > 5000) {
      hint_at_ = 0;
      paint_list();
    }
    // A row that was sent: once the speaker plays, the words are its own again; one that did not start says so.
    if (starting_) {
      const bool on = count_ && playing(speakers_[at_]);
      if ((on && now - sent_at_ > 3000) || now - sent_at_ > 12000) {
        starting_ = false;
        if (!on) failed_at_ = now;
        paint();
      }
    }
    if (failed_at_ && now - failed_at_ > 6000) {
      failed_at_ = 0;
      paint_now();
    }
    // A toggle or a volume Home Assistant did not confirm goes back to what it says.
    if (pend_mask_ && now - pend_at_ > 8000) {
      pend_mask_ = 0;
      paint_speakers();
    }
    if (vol_wish_ >= 0 && now - vol_wish_at_ > 6000) {
      vol_wish_ = -1;
      paint_volume();
    }
    paint_progress();
    if (!full_) return;
    // An answer can get lost on its way (the app drops one for a screen whose layout changed meanwhile), so a page is
    // asked again twice before the deck says there was none; after that now and then.
    if (asked_ && now - asked_at_ > 8000) {
      asked_ = 0;
      if (++tries_ < 3) {
        ask(false);
      } else {
        wait_ = Wait::NO_ANSWER;
        paint_list();
      }
    }
    if (!asked_ && wait_ == Wait::OFFLINE && now - asked_at_ > 3000) ask(false);
    if (!asked_ && (wait_ == Wait::FAILED || wait_ == Wait::NO_ANSWER) && now - asked_at_ > 20000) ask(true);
  }

  // A tap on the deck; false when it hit none of its parts (a small tile then opens the card).
  bool tap(tessera::Point p) {
    const int slack = ui::px(4);
    Speaker *s = count_ ? &speakers_[at_] : nullptr;
    if (play_.hit(p, slack)) {
      if (s) tessera::wish(s->entity, tessera::Wish::PLAYING, playing(*s) ? "0" : "1");
      return true;
    }
    if (stop_.hit(p, slack)) {
      if (s) tessera::action(STOP, s->entity);
      return true;
    }
    if (next_.hit(p, slack)) {
      if (s) tessera::action(NEXT, s->entity);
      return true;
    }
    if (previous_.hit(p, slack)) {
      if (s) tessera::action(PREVIOUS, s->entity);
      return true;
    }
    if (!full_) return false;
    if (view_.hit(p, slack)) {
      view_list_ = !view_list_;
      lay_chips();
      paint();
      return true;
    }
    if (mute_.hit(p, slack)) {
      if (s) tessera::wish(s->entity, tessera::Wish::MUTED, s->muted ? "0" : "1");
      return true;
    }
    if (vol_w_ > 0 && p.x >= vol_x_ - slack && p.x < vol_x_ + vol_w_ + slack && p.y >= vol_y_ && p.y < vol_y_ + key_) {
      if (s) set_volume(*s, std::max(0, std::min(100, (p.x - vol_x_) * 100 / vol_w_)));
      return true;
    }
    if (move_.hit(p, slack)) {
      const int from = source();
      if (s && from >= 0) {
        JsonDocument d;
        d["source_player"] = speakers_[from].entity;
        d["auto_play"] = true;
        tessera::action(TRANSFER, s->entity, d);
      }
      return true;
    }
    for (int i = 0; i < count_ && col_; ++i) {
      if (toggles_[i].hit(p, slack)) {
        toggle(i);
        return true;
      }
      if (rows_speakers_[i].hit(p, 0)) {
        choose(i);
        return true;
      }
    }
    for (int i = 0; i < count_ && count_ > 1 && !col_; ++i) {
      if (!speaker_chips_[i].hit(p, slack)) continue;
      choose(all_speakers_ ? i : (at_ + 1) % count_);
      return true;
    }
    for (int i = 0; i < setup_.filter_count; ++i) {
      if (!filter_chips_[i].hit(p, slack)) continue;
      const int next = all_filters_ ? i : (filter_ + 1) % setup_.filter_count;
      if (next != filter_ || !all_filters_) {
        filter_ = next;
        offset_ = 0;
        lay_chips();
        paint_chips();
        ask(true);
      }
      return true;
    }
    if (left_.hit(p, slack)) {
      if (offset_ > 0) {
        offset_ = std::max(0, offset_ - rows_);
        ask(true);
      }
      return true;
    }
    if (right_.hit(p, slack)) {
      if (more_) {
        offset_ += items_count_;
        ask(true);
      }
      return true;
    }
    for (int i = 0; i < rows_ && i < items_count_; ++i)
      if (rows_parts_[i].hit(p, 0)) {
        play(i);
        return true;
      }
    return true;
  }

  // An answer of the app: {"re", "ok", "result": {"items": [{"name", "uri", ...}], ...}}.
  void answer(JsonObjectConst m) {
    if (!asked_ || (m["re"] | 0u) != asked_) return;
    asked_ = 0;
    tries_ = 0;
    items_count_ = 0;
    more_ = false;
    if (!(m["ok"] | false)) {
      // Home Assistant's own words for the refusal, as the app passes them on.
      keep(error_, m["error"] | "");
      wait_ = plugin_->entry()[0] ? Wait::FAILED : Wait::NO_ENTRY;
    } else {
      // get_library answers {"items": [...], "limit", "offset", ...}; a list by its kind ("playlists") is taken too.
      JsonArrayConst list = m["result"]["items"].as<JsonArrayConst>();
      if (list.isNull()) list = m["result"][shelf(setup_.kind)].as<JsonArrayConst>();
      for (JsonObjectConst item : list) {
        if (items_count_ >= rows_) break;
        Item &it = items_[items_count_];
        keep(it.name, item["name"] | "");
        keep(it.uri, item["uri"] | "");
        if (it.name[0]) ++items_count_;
      }
      more_ = items_count_ >= rows_;
      wait_ = items_count_ ? Wait::NONE : Wait::EMPTY;
    }
    paint_list();
    tessera::refresh();
  }

  void paint() {
    paint_now();
    paint_keys();
    if (!full_) return;
    paint_volume();
    paint_speakers();
    paint_chips();
    paint_list();
  }

 private:
  enum class Wait : uint8_t { NONE, LOADING, EMPTY, FAILED, NO_ENTRY, NO_ANSWER, OFFLINE };
  struct Item {
    char name[52] = {};
    char uri[52] = {};
  };

  // ---- What Home Assistant says ----

  void take(Speaker &s, JsonObjectConst e) {
    keep(s.state, e["state"] | "");
    keep(s.name, e["name"] | "");
    JsonObjectConst a = e["attributes"].as<JsonObjectConst>();
    keep(s.title, a["media_title"] | "");
    keep(s.artist, a["media_artist"] | "");
    s.volume = a["volume_level"].is<float>() ? static_cast<int16_t>(std::lround(a["volume_level"].as<float>() * 100)) : -1;
    s.muted = a["is_volume_muted"] | false;
    s.position = a["media_position"].is<float>() ? static_cast<int32_t>(a["media_position"].as<float>()) : -1;
    s.duration = a["media_duration"].is<float>() ? static_cast<int32_t>(a["media_duration"].as<float>()) : -1;
    s.updated = a["media_position_updated_at"] | 0u;
    s.members = 0;
    for (JsonVariantConst m : a["group_members"].as<JsonArrayConst>()) {
      const char *id = m | "";
      for (int k = 0; k < count_; ++k)
        if (!strcmp(speakers_[k].entity, id)) s.members |= 1u << k;
    }
  }

  // A toggle or a volume Home Assistant now says as the finger set it needs no wish of its own any more.
  void settle() {
    for (int i = 0; i < count_; ++i) {
      const uint8_t bit = 1u << i;
      if ((pend_mask_ & bit) && grouped(i) == bool(pend_on_ & bit)) pend_mask_ &= ~bit;
    }
    if (vol_wish_ >= 0 && count_ && std::abs(speakers_[at_].volume - vol_wish_) <= 1) vol_wish_ = -1;
  }

  static bool playing(const Speaker &s) { return !strcmp(s.state, "playing") || !strcmp(s.state, "buffering"); }
  static bool active(const Speaker &s) { return playing(s) || !strcmp(s.state, "paused"); }
  static bool off(const Speaker &s) { return !strcmp(s.state, "off") || !strcmp(s.state, "unavailable"); }
  const char *name_of(const Speaker &s) const { return s.name[0] ? s.name : s.entity; }
  const Speaker *chosen() const { return count_ ? &speakers_[at_] : nullptr; }
  bool list_on() const { return three_ || view_list_; }
  bool now_on() const { return !full_ || three_ || !view_list_; }

  // Whether speaker i plays along with the chosen one, as Home Assistant says it (group_members) or a finger wished.
  bool grouped(int i) const {
    if (i == at_) return true;
    const uint8_t bit = 1u << i;
    return (speakers_[at_].members & bit) || (speakers_[i].members & (1u << at_));
  }
  bool joined(int i) const {
    const uint8_t bit = 1u << i;
    if (i != at_ && (pend_mask_ & bit)) return pend_on_ & bit;
    return grouped(i);
  }
  // A speaker that plays apart from the chosen one: where Move here takes the music from.
  int source() const {
    for (int i = 0; i < count_; ++i)
      if (i != at_ && playing(speakers_[i]) && !joined(i)) return i;
    return -1;
  }

  // ---- What a finger does ----

  void choose(int i) {
    if (i == at_) return;
    at_ = i;
    pend_mask_ = 0;
    vol_wish_ = -1;
    lay_chips();
    paint();
  }

  // A speaker's toggle: it plays along with the chosen one (media_player.join on the chosen one), or no more
  // (media_player.unjoin on itself). Music Assistant groups the players it can play in sync.
  void toggle(int i) {
    if (i == at_ || !count_) return;
    const bool on = joined(i);
    bool sent;
    if (on) {
      sent = tessera::action(UNJOIN, speakers_[i].entity);
    } else {
      JsonDocument d;
      d["group_members"].to<JsonArray>().add(speakers_[i].entity);
      sent = tessera::action(JOIN, speakers_[at_].entity, d);
    }
    if (!sent) return;
    const uint8_t bit = 1u << i;
    pend_mask_ |= bit;
    if (on) pend_on_ &= ~bit;
    else pend_on_ |= bit;
    pend_at_ = esphome::millis();
    paint_speakers();
  }

  void set_volume(const Speaker &s, int level) {
    JsonDocument d;
    d["volume_level"] = level / 100.0f;
    if (!tessera::action(VOLUME, s.entity, d)) return;
    vol_wish_ = level;
    vol_wish_at_ = esphome::millis();
    paint_volume();
  }

  void ask(bool fresh) {
    if (fresh) tries_ = 0;
    asked_at_ = esphome::millis();
    const Filter &f = FILTER[setup_.filters[filter_]];
    JsonDocument r;
    r["ask"] = LIBRARY;
    JsonObject d = r["data"].to<JsonObject>();
    // Without the entry Home Assistant refuses the action; the refusal then says to fill it in.
    if (plugin_->entry()[0]) d["config_entry_id"] = plugin_->entry();
    d["media_type"] = setup_.kind;
    d["limit"] = rows_;
    d["offset"] = offset_;
    d["order_by"] = f.order;
    if (f.favorite) d["favorite"] = true;
    if (setup_.contains[0]) d["search"] = setup_.contains;
    asked_ = tessera::send(plugin_, r.as<JsonObjectConst>());
    if (fresh || !asked_) wait_ = asked_ ? Wait::LOADING : Wait::OFFLINE;
    paint_list();
  }

  void play(int i) {
    const Speaker *s = chosen();
    if (!s) {
      // Nowhere to play: say where a player is chosen, for a few seconds.
      hint_at_ = esphome::millis();
      paint_list();
      return;
    }
    const Item &it = items_[i];
    // The row stays lit as the one picked until another is, across pages and answers asked again.
    keep(picked_, it.name);
    JsonDocument d;
    d["media_id"] = !it.uri[0] || maybe_cut(it.uri) ? it.name : it.uri;
    d["media_type"] = setup_.kind;
    d["enqueue"] = setup_.enqueue;
    if (tessera::action(PLAY, s->entity, d)) {
      starting_ = true;
      keep(started_, it.name);
      sent_at_ = esphome::millis();
      failed_at_ = 0;
      // A stacked deck turns to what plays.
      if (!three_) view_list_ = false;
      lay_chips();
    } else {
      // Not sent (the screen may not perform actions now): say so instead of nothing.
      keep(started_, it.name);
      failed_at_ = esphome::millis();
    }
    paint();
  }

  // ---- The layouts ----

  void make_three(lv_obj_t *p) {
    int x = 0;
    if (count_ > 1) {
      const int sw = std::max(ui::mm(32), std::min(ui::mm(50), w_ / 5));
      make_speakers(p, 0, 0, sw, h_);
      x = sw + 2 * gap_;
    }
    const int rest = w_ - x;
    const int cw = std::min(rest / 2, std::max(ui::mm(60), rest * 9 / 20));
    const int used = make_controls(p, x, h_, cw, true, false);
    make_now(p, x, 0, cw, h_ - used - gap_);
    const int lx = x + cw + 2 * gap_;
    make_library(p, lx, 0, w_ - lx, h_);
  }

  void make_stack(lv_obj_t *p) {
    int x = 0;
    col_ = count_ > 1 && w_ >= ui::mm(62);
    if (col_) {
      const int sw = std::max(ui::mm(22), std::min(ui::mm(40), w_ * 3 / 10));
      make_speakers(p, 0, 0, sw, h_);
      x = sw + gap_;
    }
    const int pw = w_ - x;
    int top = 0;
    if (!col_ && count_ > 1) {
      chips_x_ = x;
      chips_room_ = pw;
      for (int i = 0; i < count_; ++i) make_chip(speaker_chips_[i], p);
      top = key_ + gap_;
    }
    const int used = make_controls(p, x, h_, pw, h_ >= ui::mm(55), true);
    const int room = h_ - used - gap_ - top;
    make_now(p, x, top, pw, room);
    make_library(p, x, top, pw, room);
  }

  // The speakers down the left: a row each (a tap chooses it), with a toggle at its right for every speaker but the
  // chosen one (a tap lets it play along, or no more).
  void make_speakers(lv_obj_t *p, int x, int y, int w, int h) {
    col_ = true;
    int rh = std::max(ui::mm(9), ui::line_height(Font::BODY_LARGE) + ui::px(16));
    rh = std::max(key_, std::min(rh, (h - (count_ - 1) * gap_) / std::max(1, count_)));
    const int tog = std::max(ui::mm(6), rh - ui::px(12));
    const bool icon = w >= ui::mm(35);
    const Font font = w >= ui::mm(35) ? Font::BODY_LARGE : Font::BODY;
    const int pad = ui::px(10);
    const int iw = icon ? ui::line_height(Font::ICON_SMALL) + ui::px(6) : 0;
    spk_text_w_ = w - 2 * pad - iw - tog - ui::px(6);
    spk_h_ = rh;
    spk_font_ = font;
    for (int i = 0; i < count_; ++i) {
      const int ry = y + i * (rh + gap_);
      Part &r = rows_speakers_[i];
      r.box = still_block(p, theme::RAISED);
      r.place(x, ry, w, rh);
      if (icon) {
        icons_[i] = ui::label(r.box, Font::ICON_SMALL, theme::MUTED);
        lv_obj_set_pos(icons_[i], pad, (rh - ui::line_height(Font::ICON_SMALL)) / 2);
      }
      r.label = ui::text(r.box, font, theme::INK);
      lv_obj_set_pos(r.label, pad + iw, 0);
      r.show(true);
      // The toggle lies in its row: placed in the row's pixels, hit in the deck's.
      Part &t = toggles_[i];
      t.box = still_block(r.box, theme::TRACK);
      lv_obj_set_style_radius(t.box, LV_RADIUS_CIRCLE, 0);
      t.label = ui::label(t.box, Font::ICON_SMALL, theme::INK);
      t.place(w - tog - ui::px(6), (rh - tog) / 2, tog, tog);
      t.x += x;
      t.y += ry;
      lv_obj_center(t.label);
      t.show(false);
    }
  }

  // The playing keys centred at the bottom (previous, stop, play, next; play larger), the volume under them, and in a
  // stacked deck the key that switches between what plays and the library at their right. Returns the height taken.
  int make_controls(lv_obj_t *p, int x, int bottom, int w, bool volume, bool view) {
    int big = std::max(ui::mm(11), ui::line_height(Font::ICON) + ui::px(16));
    const int lift = ui::px(12);
    auto width = [&](int b) { return 4 * b + lift + 3 * gap_ + (view ? b + 2 * gap_ : 0); };
    while (big > key_ && width(big) > w) big -= ui::px(2);
    // Play a size larger than its neighbours, where the row has the room for it.
    const int play = width(big) <= w ? big + lift : big;
    const Font font = ui::line_height(Font::ICON) + ui::px(10) <= big ? Font::ICON : Font::ICON_SMALL;
    int y = bottom;
    if (volume) {
      y -= key_;
      make_volume(p, x, y, w);
      y -= gap_;
    }
    const int ty = y - play;
    const int row = 3 * big + play + 3 * gap_;
    int sx = x + (w - row) / 2;
    if (view) {
      make_key(view_, p, big, ICON_LIST, font);
      view_.place(x + w - big, ty + (play - big) / 2, big, big);
      sx = std::max(x, std::min(sx, x + w - big - 2 * gap_ - row));
    }
    const int ky = ty + (play - big) / 2;
    make_key(previous_, p, big, ICON_PREVIOUS, font);
    previous_.place(sx, ky, big, big);
    make_key(stop_, p, big, ICON_STOP, font);
    stop_.place(sx + big + gap_, ky, big, big);
    make_key(play_, p, play, ICON_PLAY, font);
    play_.place(sx + 2 * (big + gap_), ty, play, play);
    make_key(next_, p, big, ICON_NEXT, font);
    next_.place(sx + 2 * (big + gap_) + play + gap_, ky, big, big);
    return bottom - ty;
  }

  // The volume of the chosen speaker: its mute key, the bar (a tap sets the level where it lands) and the level.
  void make_volume(lv_obj_t *p, int x, int y, int w) {
    make_key(mute_, p, key_, ICON_VOLUME);
    mute_.place(x, y, key_, key_);
    const int pw = ui::text_width("100%", Font::BODY) + ui::px(6);
    vol_pct_ = ui::label(p, Font::BODY, theme::MUTED);
    lv_obj_set_width(vol_pct_, pw);
    lv_obj_set_style_text_align(vol_pct_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(vol_pct_, x + w - pw, y + (key_ - ui::line_height(Font::BODY)) / 2);
    const int th = ui::px(10);
    vol_x_ = x + key_ + gap_;
    vol_y_ = y;
    vol_w_ = std::max(ui::px(20), w - key_ - 2 * gap_ - pw);
    vol_track_ = bar_block(p, theme::TRACK, vol_x_, y + (key_ - th) / 2, vol_w_, th);
    vol_fill_ = bar_block(p, theme::ACCENT, vol_x_, y + (key_ - th) / 2, th, th);
  }

  // What plays: the cover (a music icon under it), the title and the artist, and the progress with its times, or in its
  // place a key that moves the music here from a speaker that plays it elsewhere. The cover beside the words where the
  // room lies down, else over them; all in the middle of the room.
  void make_now(lv_obj_t *p, int x, int y, int w, int h) {
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY);
    // The times under the bar, or beside it where the room is short (a card on a small screen).
    int prog = std::max(key_, ui::px(12) + bh);
    if (th + bh + gap_ + prog > h) {
      inline_ = true;
      prog = bh + ui::px(8);
    }
    const int words = th + bh + gap_ + prog;
    if (w >= 2 * h) {
      const int side = std::min(h, w * 2 / 5);
      art(p, x, y + (h - side) / 2, side);
      const int tx = x + side + 2 * gap_, tw = w - side - 2 * gap_;
      const int ty = y + std::max(0, (h - words) / 2);
      make_words(p, tx, ty, tw, false);
      make_progress(p, tx, ty + th + bh + gap_, tw, prog);
    } else {
      int side = std::min(w, h - words - gap_);
      if (side < ui::mm(12)) side = 0;
      const int total = (side ? side + gap_ : 0) + words;
      const int top = y + std::max(0, (h - total) / 2);
      art(p, x + (w - side) / 2, top, side);
      const int ty = top + (side ? side + gap_ : 0);
      make_words(p, x, ty, w, true);
      make_progress(p, x, ty + th + bh + gap_, w, prog);
    }
  }

  void make_words(lv_obj_t *p, int x, int y, int w, bool centred) {
    title_ = ui::label(p, Font::TITLE, theme::INK);
    lv_obj_set_width(title_, w);
    lv_obj_set_pos(title_, x, y);
    artist_ = ui::label(p, Font::BODY, theme::MUTED);
    lv_obj_set_width(artist_, w);
    lv_obj_set_pos(artist_, x, y + ui::line_height(Font::TITLE));
    if (centred) {
      lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_align(artist_, LV_TEXT_ALIGN_CENTER, 0);
    }
  }

  void make_progress(lv_obj_t *p, int x, int y, int w, int h) {
    const int bar = ui::px(6), bh = ui::line_height(Font::BODY);
    int bx = x, bw = w, by, ty, tw = w / 2;
    if (inline_) {
      // 1:14 ===== 3:30 on one line.
      tw = ui::text_width("00:00", Font::BODY) + ui::px(8);
      bx = x + tw;
      bw = std::max(ui::px(20), w - 2 * tw);
      by = y + (h - bar) / 2;
      ty = y + (h - bh) / 2;
    } else {
      by = y + (h - bar - ui::px(6) - bh) / 2;
      ty = by + bar + ui::px(6);
    }
    prog_w_ = bw;
    prog_track_ = bar_block(p, theme::TRACK, bx, by, bw, bar);
    prog_fill_ = bar_block(p, theme::ACCENT, bx, by, bar, bar);
    time_left_ = ui::label(p, Font::BODY, theme::MUTED);
    lv_obj_set_width(time_left_, inline_ ? tw - ui::px(6) : tw);
    lv_obj_set_pos(time_left_, x, ty);
    time_right_ = ui::label(p, Font::BODY, theme::MUTED);
    lv_obj_set_width(time_right_, inline_ ? tw - ui::px(6) : tw);
    lv_obj_set_style_text_align(time_right_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(time_right_, x + w - (inline_ ? tw - ui::px(6) : tw), ty);
    make_chip(move_, p);
    move_mid_ = x + w / 2;
    move_h_ = std::min(key_, h);
    move_y_ = y + (h - move_h_) / 2;
    move_room_ = w;
  }

  // The library: the filters with the page keys at their right, a page of rows under them, and a note in their middle
  // while there are none.
  void make_library(lv_obj_t *p, int x, int y, int w, int h) {
    lib_x_ = x;
    lib_w_ = w;
    filters_y_ = y;
    for (int i = 0; i < setup_.filter_count; ++i) make_chip(filter_chips_[i], p);
    make_key(left_, p, key_, ICON_LEFT);
    make_key(right_, p, key_, ICON_RIGHT);
    right_.place(x + w - key_, y, key_, key_);
    left_.place(x + w - 2 * key_ - gap_, y, key_, key_);
    const int ly = y + key_ + gap_;
    rows_ = std::max(1, std::min(ROWS, (y + h - ly + row_gap_) / (row_h_ + row_gap_)));
    for (int i = 0; i < rows_; ++i) {
      Part &r = rows_parts_[i];
      r.box = still_block(p, theme::RAISED);
      r.label = ui::label(r.box, Font::BODY_LARGE, theme::INK);
      r.place(x, ly + i * (row_h_ + row_gap_), w, row_h_);
      lv_obj_set_width(r.label, w - 2 * ui::px(12));
      lv_obj_set_pos(r.label, ui::px(12), (row_h_ - ui::line_height(Font::BODY_LARGE)) / 2);
      r.show(false);
    }
    note_ = ui::text(p, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, w, 2 * ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, x, ly + std::max(0, (y + h - ly - 2 * ui::line_height(Font::BODY)) / 2));
  }

  // A small tile: one lying down is a row of what plays over its height; a square or standing one is the cover with the
  // play key under it, the two in the middle of the height (the key alone where no cover fits over it).
  void make_small(lv_obj_t *p) {
    if (w_ * 2 > h_ * 3) {
      make_row(p);
      return;
    }
    int side = std::min(w_, h_ - key_ - gap_);
    if (side < ui::mm(8)) side = 0;
    const int top = (h_ - (side ? side + gap_ : 0) - key_) / 2;
    art(p, (w_ - side) / 2, top, side);
    make_key(play_, p, key_, ICON_PLAY);
    play_.place((w_ - key_) / 2, top + (side ? side + gap_ : 0), key_, key_);
  }

  void make_row(lv_obj_t *p) {
    const int text_min = ui::mm(14);
    int side = h_;
    if (w_ < side + gap_ + key_ + text_min) side = 0;
    art(p, 0, 0, side);
    int right = w_;
    const bool all = w_ - side - gap_ - text_min >= 3 * key_ + 2 * gap_;
    const int ky = (h_ - key_) / 2;
    if (all) {
      make_key(next_, p, key_, ICON_NEXT);
      next_.place(right - key_, ky, key_, key_);
      right -= key_ + gap_;
    }
    make_key(play_, p, key_, ICON_PLAY);
    play_.place(right - key_, ky, key_, key_);
    right -= key_ + gap_;
    if (all) {
      make_key(previous_, p, key_, ICON_PREVIOUS);
      previous_.place(right - key_, ky, key_, key_);
      right -= key_ + gap_;
    }
    const int x = side ? side + gap_ : 0;
    if (right + gap_ - x < text_min) return;
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY);
    make_words(p, x, (h_ - th - bh) / 2, right - x, false);
  }

  // The square of the cover: a music icon, and over it the cover where the screen shows pictures.
  void art(lv_obj_t *p, int x, int y, int side) {
    if (side <= 0) return;
    art_ = still_block(p, theme::TRACK);
    lv_obj_set_size(art_, side, side);
    lv_obj_set_pos(art_, x, y);
    art_icon_ = ui::label(art_, Font::ICON, theme::MUTED);
    ui::set_text(art_icon_, ui::icon(ICON_MUSIC));
    lv_obj_center(art_icon_);
    if (!pictures_) return;
    // No block of its own while the cover comes: the icon shows through until it is there.
    cover_ = ui::picture(p, tessera::Placeholder::NONE);
    lv_obj_set_size(cover_, side, side);
    lv_obj_set_pos(cover_, x, y);
  }

  // The chips of one row from `x` up to `room` wide: every word when they all fit, else one chip, the chosen one,
  // which a tap steps on. True when they all fit.
  bool lay_row(Part *parts, int n, const std::string *words, int chosen, int x, int y, int room, bool visible) {
    const int pad = ui::px(12);
    auto width = [&](const std::string &word) { return ui::text_width(word, Font::BODY) + 2 * pad; };
    int need = 0;
    for (int i = 0; i < n; ++i) need += width(words[i]) + (i ? gap_ : 0);
    const bool all = need <= room;
    for (int i = 0; i < n; ++i) {
      Part &c = parts[i];
      if (!c.box) continue;
      if (!visible || (!all && i)) {
        c.show(false);
        continue;
      }
      const std::string &word = all ? words[i] : words[chosen];
      const int cw = std::min(width(word), room);
      c.place(x, y, cw, key_);
      lv_obj_set_style_radius(c.box, key_ / 2, 0);
      lv_obj_set_width(c.label, cw - 2 * pad);
      lv_obj_set_pos(c.label, pad, (key_ - ui::line_height(Font::BODY)) / 2);
      ui::set_text(c.label, word);
      c.show(true);
      x += cw + gap_;
    }
    return all;
  }

  void lay_chips() {
    if (!full_) return;
    if (count_ > 1 && !col_) {
      std::string words[SPEAKERS];
      for (int i = 0; i < count_; ++i) words[i] = name_of(speakers_[i]);
      all_speakers_ = lay_row(speaker_chips_, count_, words, at_, chips_x_, 0, chips_room_, true);
    }
    std::string words[FILTERS];
    for (int i = 0; i < setup_.filter_count; ++i) words[i] = plugin_->text(FILTER[setup_.filters[i]].word);
    all_filters_ = lay_row(filter_chips_, setup_.filter_count, words, filter_, lib_x_, filters_y_,
                           lib_w_ - 2 * key_ - 2 * gap_, list_on());
  }

  // ---- Painting ----

  void paint_now() {
    const Speaker *s = chosen();
    const bool on = s && active(*s);
    const bool here = now_on();
    for (lv_obj_t *o : {art_, title_, artist_}) shown(o, here);
    if (title_) {
      std::string top, under;
      const int from = s ? source() : -1;
      if (!s) {
        top = plugin_->text("no_player");
      } else if (failed_at_) {
        top = started_;
        under = tessera::fill(plugin_->text("not_started"), "player", name_of(*s));
      } else if (starting_) {
        top = started_;
        under = tessera::fill(plugin_->text("starting"), "player", name_of(*s));
      } else if (on && s->title[0]) {
        top = s->title;
        under = s->artist[0] ? s->artist : name_of(*s);
      } else {
        top = plugin_->text("nothing");
        under = from >= 0 ? tessera::fill(plugin_->text("elsewhere"), "player", name_of(speakers_[from]))
                : off(*s) ? tessera::fill(plugin_->text("is_off"), "player", name_of(*s))
                          : name_of(*s);
      }
      ui::set_text(title_, top);
      ui::set_text(artist_, under);
      ui::set_color(title_, theme::INK);
      ui::set_color(artist_, failed_at_ ? theme::INK : theme::MUTED);
    }
    if (art_) {
      ui::set_fill(art_, Tone::NORMAL, theme::TRACK);
      ui::set_color(art_icon_, theme::MUTED);
    }
    // The cover of the tile's own player: the app paints Home Assistant's pictures of the tile's (or the card's) own
    // entity only. Another speaker shows it too while it plays along with that player. With no address the place shows
    // nothing, and the music icon under it shows through.
    if (cover_) {
      std::string address;
      const Speaker &own = speakers_[0];
      if (here && s && on && own.option < 0 && active(own) && (s == &own || !strcmp(s->title, own.title)))
        address = std::string("cover:") + own.entity;
      ui::set_picture(cover_, address, tessera::Fit::COVER);
    }
    // Move here: in the progress's place, while the chosen speaker is quiet and another plays on its own.
    if (move_.box) {
      const bool move = here && s && !on && !starting_ && source() >= 0;
      if (move) {
        const std::string word = plugin_->text("move_here");
        const int pad = ui::px(14);
        const int cw = std::min(ui::text_width(word, Font::BODY) + 2 * pad, move_room_);
        move_.place(move_mid_ - cw / 2, move_y_, cw, move_h_);
        lv_obj_set_style_radius(move_.box, move_h_ / 2, 0);
        lv_obj_set_width(move_.label, cw - 2 * pad);
        lv_obj_set_pos(move_.label, pad, (move_h_ - ui::line_height(Font::BODY)) / 2);
        ui::set_text(move_.label, word);
        ui::set_fill(move_.box, Tone::ACCENT, theme::ACCENT);
        ui::set_color(move_.label, theme::ON_ACCENT);
      }
      move_.show(move);
    }
    paint_progress();
  }

  // The bar and the times, once a second: the position Home Assistant last said, plus the time since while it plays.
  void paint_progress() {
    if (!prog_track_) return;
    const Speaker *s = chosen();
    const bool show = now_on() && s && active(*s) && s->duration > 0 && !starting_;
    for (lv_obj_t *o : {prog_track_, prog_fill_, time_left_, time_right_}) shown(o, show);
    if (!show) return;
    int32_t at = std::max<int32_t>(0, s->position);
    if (playing(*s) && s->updated && epoch_ > s->updated) at += static_cast<int32_t>(epoch_ - s->updated);
    at = std::min(at, s->duration);
    const int fw = static_cast<int>(static_cast<int64_t>(prog_w_) * at / s->duration);
    lv_obj_set_width(prog_fill_, std::max(fw, ui::px(6)));
    ui::set_fill(prog_track_, Tone::NORMAL, surface_);
    ui::set_fill(prog_fill_, Tone::ACCENT, theme::ACCENT);
    ui::set_text(time_left_, clock_of(at));
    ui::set_text(time_right_, clock_of(s->duration));
  }

  void paint_keys() {
    const Speaker *s = chosen();
    if (play_.box) {
      ui::set_text(play_.label, ui::icon(s && playing(*s) ? ICON_PAUSE : ICON_PLAY));
      ui::set_fill(play_.box, Tone::ACCENT, theme::ACCENT);
      ui::set_color(play_.label, theme::ON_ACCENT);
    }
    for (Part *k : {&previous_, &stop_, &next_, &view_}) {
      if (!k->box) continue;
      ui::set_fill(k->box, Tone::NORMAL, surface_);
      ui::set_color(k->label, theme::INK);
    }
    if (view_.box) ui::set_text(view_.label, ui::icon(view_list_ ? ICON_MUSIC : ICON_LIST));
  }

  void paint_volume() {
    if (!vol_track_) return;
    const Speaker *s = chosen();
    const int level = vol_wish_ >= 0 ? vol_wish_ : (s ? s->volume : -1);
    const bool muted = s && s->muted;
    ui::set_fill(vol_track_, Tone::NORMAL, surface_);
    const int fw = level > 0 ? vol_w_ * level / 100 : 0;
    lv_obj_set_width(vol_fill_, std::max(fw, ui::px(10)));
    shown(vol_fill_, fw > 0);
    ui::set_fill(vol_fill_, muted ? Tone::NORMAL : Tone::ACCENT, theme::SUBTLE);
    ui::set_text(vol_pct_, level >= 0 ? std::to_string(level) + "%" : "");
    ui::set_color(vol_pct_, theme::MUTED);
    ui::set_text(mute_.label, ui::icon(muted ? ICON_MUTED : ICON_VOLUME));
    ui::set_fill(mute_.box, Tone::NORMAL, surface_);
    ui::set_color(mute_.label, muted ? theme::MUTED : theme::INK);
  }

  void paint_speakers() {
    if (!col_) return;
    const int line = ui::line_height(spk_font_);
    for (int i = 0; i < count_; ++i) {
      Part &r = rows_speakers_[i];
      if (!r.box) continue;
      const Speaker &s = speakers_[i];
      const bool chosen_one = i == at_;
      ui::set_fill(r.box, chosen_one ? Tone::ACCENT : Tone::NORMAL, surface_);
      // The name on one line where it fits, else on two (cut with dots after the second).
      const std::string name = name_of(s);
      const int lines = ui::text_width(name, spk_font_) <= spk_text_w_ || spk_h_ < 2 * line + ui::px(4) ? 1 : 2;
      lv_obj_set_size(r.label, spk_text_w_, lines * line);
      lv_obj_set_y(r.label, (spk_h_ - lines * line) / 2);
      ui::set_text(r.label, name);
      ui::set_color(r.label, chosen_one ? theme::ON_ACCENT : theme::INK);
      if (icons_[i]) {
        ui::set_text(icons_[i], ui::icon(off(s) ? ICON_SPEAKER_OFF : ICON_SPEAKER));
        ui::set_color(icons_[i], chosen_one ? theme::ON_ACCENT : playing(s) ? theme::ACCENT : theme::MUTED);
      }
      Part &t = toggles_[i];
      const bool toggle = count_ > 1 && !chosen_one;
      t.show(toggle);
      if (!toggle) continue;
      const bool along = joined(i);
      ui::set_text(t.label, ui::icon(along ? ICON_JOINED : ICON_ADD));
      ui::set_fill(t.box, along ? Tone::ACCENT : Tone::NORMAL, surface_ == theme::RAISED ? theme::TRACK : theme::RAISED);
      ui::set_color(t.label, along ? theme::ON_ACCENT : theme::INK);
    }
  }

  void paint_chip(Part &c, bool lit) {
    if (!c.box) return;
    ui::set_fill(c.box, lit ? Tone::ACCENT : Tone::NORMAL, surface_);
    ui::set_color(c.label, lit ? theme::ON_ACCENT : theme::INK);
  }

  void paint_chips() {
    for (int i = 0; i < count_ && count_ > 1 && !col_; ++i) paint_chip(speaker_chips_[i], !all_speakers_ || i == at_);
    for (int i = 0; i < setup_.filter_count; ++i) paint_chip(filter_chips_[i], !all_filters_ || i == filter_);
  }

  void paint_list() {
    if (!full_) return;
    const bool here = list_on();
    for (int i = 0; i < rows_; ++i) {
      Part &r = rows_parts_[i];
      const bool there = here && wait_ == Wait::NONE && !hint_at_ && i < items_count_;
      r.show(there);
      if (!there) continue;
      ui::set_text(r.label, items_[i].name);
      const bool lit = picked_[0] && strcmp(items_[i].name, picked_) == 0;
      ui::set_fill(r.box, lit ? Tone::ACCENT : Tone::NORMAL, surface_);
      ui::set_color(r.label, lit ? theme::ON_ACCENT : theme::INK);
    }
    const char *why = wait_ == Wait::LOADING ? "loading" : wait_ == Wait::EMPTY ? "empty"
                      : wait_ == Wait::FAILED ? "failed" : wait_ == Wait::NO_ENTRY ? "no_entry"
                      : wait_ == Wait::NO_ANSWER ? "no_answer" : wait_ == Wait::OFFLINE ? "offline" : nullptr;
    std::string note = why ? plugin_->text(why) : "";
    if (wait_ == Wait::FAILED) note = tessera::fill(note.c_str(), "reason", error_[0] ? error_ : "?");
    if (hint_at_) note = plugin_->text("pick_player");
    shown(note_, here);
    ui::set_text(note_, note);
    ui::set_color(note_, theme::MUTED);
    // The page keys: lit where they lead somewhere.
    left_.show(here);
    right_.show(here);
    ui::set_fill(left_.box, Tone::NORMAL, surface_);
    ui::set_fill(right_.box, Tone::NORMAL, surface_);
    ui::set_color(left_.label, offset_ > 0 ? theme::INK : theme::SUBTLE);
    ui::set_color(right_.label, more_ ? theme::INK : theme::SUBTLE);
  }

  MusicDeck *plugin_;
  Setup setup_;
  Speaker speakers_[SPEAKERS];
  Item items_[ROWS];
  char started_[52] = {};
  char picked_[52] = {};   // the name of the row last tapped, lit while it is on the page
  char error_[124] = {};
  int count_ = 0, at_ = 0, filter_ = 0, offset_ = 0, items_count_ = 0, rows_ = 0, tries_ = 0;
  int w_ = 0, h_ = 0, key_ = 0, gap_ = 0, row_h_ = 0, row_gap_ = 0;
  int lib_x_ = 0, lib_w_ = 0, filters_y_ = 0, chips_x_ = 0, chips_room_ = 0;
  int spk_h_ = 0, spk_text_w_ = 0, prog_w_ = 0, move_mid_ = 0, move_y_ = 0, move_h_ = 0, move_room_ = 0;
  int vol_x_ = 0, vol_y_ = 0, vol_w_ = 0, vol_wish_ = -1;
  Font spk_font_ = Font::BODY;
  bool full_ = false, three_ = false, col_ = false, view_list_ = true, stated_ = false, starting_ = false, inline_ = false;
  bool pictures_ = false, more_ = false, all_speakers_ = true, all_filters_ = true;
  uint8_t pend_mask_ = 0, pend_on_ = 0;
  Wait wait_ = Wait::NONE;
  theme::Role surface_ = theme::RAISED;
  uint32_t asked_ = 0, asked_at_ = 0, sent_at_ = 0, hint_at_ = 0, failed_at_ = 0, pend_at_ = 0, vol_wish_at_ = 0;
  uint32_t epoch_ = 0;
  Part play_, stop_, next_, previous_, view_, mute_, move_, left_, right_;
  Part rows_speakers_[SPEAKERS], toggles_[SPEAKERS], speaker_chips_[SPEAKERS], filter_chips_[FILTERS], rows_parts_[ROWS];
  lv_obj_t *icons_[SPEAKERS] = {};
  lv_obj_t *title_{}, *artist_{}, *art_{}, *art_icon_{}, *cover_{}, *note_{};
  lv_obj_t *prog_track_{}, *prog_fill_{}, *time_left_{}, *time_right_{}, *vol_track_{}, *vol_fill_{}, *vol_pct_{};
};

// The tile: the whole deck when it has the room, else what plays; a tap beside its keys opens the deck as a card.
class DeckTile : public tessera::Tile {
 public:
  explicit DeckTile(MusicDeck *plugin) : plugin_(plugin) {}
  ~DeckTile() override { delete deck_; }

  void create(const tessera::TileContext &c) override {
    tile_ = c.tile;
    deck_ = new Deck(plugin_, setup_of(c.options), c.entity);
    deck_->make(c.parent, c.width, c.height, false);
  }
  void on_state(JsonObjectConst data) override { deck_->state(data); }
  void on_tick(uint32_t epoch) override { deck_->tick(epoch); }
  void on_theme() override { deck_->paint(); }
  void on_tap() override {
    if (deck_->tap(tessera::tap_point())) return;
    deck_->tell(plugin_->handoff());
    tessera::open_card(plugin_->plugin_id(), "deck", deck_->own(), tile_);
  }

 private:
  MusicDeck *plugin_;
  Deck *deck_ = nullptr;
  int tile_ = -1;
};

// The card: the whole deck over the glass, with the setup and the speakers of the tile it was opened from.
class DeckCard : public tessera::Card {
 public:
  explicit DeckCard(MusicDeck *plugin) : plugin_(plugin) {}
  ~DeckCard() override { delete deck_; }

  void open(const tessera::CardContext &c) override {
    const Handoff &h = plugin_->handoff();
    deck_ = new Deck(plugin_, h.setup, c.entity);
    deck_->know(h);
    deck_->make(c.parent, c.width, c.height, true);
  }
  void on_state(JsonObjectConst data) override { deck_->state(data); }
  void on_tick(uint32_t epoch) override { deck_->tick(epoch); }
  void on_theme() override { deck_->paint(); }
  void on_tap() override { deck_->tap(tessera::tap_point()); }

 private:
  MusicDeck *plugin_;
  Deck *deck_ = nullptr;
};

void MusicDeck::setup() {
  add_tile("deck", [this]() { return new DeckTile(this); });
  add_card("deck", [this]() { return new DeckCard(this); }, true);
}

void MusicDeck::on_message(JsonObjectConst message) {
  for (Deck *deck : decks_)
    if (deck) deck->answer(message);
}

void MusicDeck::join(Deck *deck) {
  for (Deck *&slot : decks_)
    if (!slot) {
      slot = deck;
      return;
    }
}

void MusicDeck::leave(Deck *deck) {
  for (Deck *&slot : decks_)
    if (slot == deck) slot = nullptr;
}

Handoff &MusicDeck::handoff() {
  if (!handoff_) handoff_ = new Handoff();
  return *handoff_;
}

}  // namespace esphome::music_deck

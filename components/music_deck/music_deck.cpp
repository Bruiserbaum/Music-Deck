#include "music_deck.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include "esphome/core/hal.h"

namespace esphome::music_deck {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// A page of the library: Music Assistant's own action, asked through the app (permissions.ha_commands).
static const char *const LIBRARY = "call_service:music_assistant.get_library";
// What plays a row: Music Assistant finds it by its library address, or by its name when the address came cut.
static const char *const PLAY = "music_assistant.play_media";
static const char *const NEXT = "media_player.media_next_track";
static const char *const PREVIOUS = "media_player.media_previous_track";

static const uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD, ICON_PREVIOUS = 0xF04AE;
static const uint32_t ICON_LEFT = 0xF0141, ICON_RIGHT = 0xF0142, ICON_MUSIC = 0xF075A;

// The option ids of the extra speakers, in the order of Setup::extra.
static const char *const SPEAKER_OPTIONS[SPEAKERS - 1] = {"speaker_2", "speaker_3", "speaker_4"};

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

// Where Music Assistant's answer files a kind: {"playlists": [...], "albums": [...], "radio": [...]}.
static std::string shelf(const char *kind) { return !strcmp(kind, "radio") ? "radio" : std::string(kind) + "s"; }

// The app cuts a text at 48 bytes without a mark: an address that long may have lost its end.
static bool maybe_cut(const char *text) { return strlen(text) >= 47; }

// A copy into a fixed field, cut where it ends.
template <size_t N> static void keep(char (&field)[N], const char *text) { snprintf(field, N, "%s", text ? text : ""); }

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
    shown = on;
    if (!box) return;
    if (on) lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  }
};

static lv_obj_t *still_block(lv_obj_t *parent, theme::Role fill) {
  lv_obj_t *o = ui::block(parent, fill);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

// A round key with an icon in its middle.
static void make_key(Part &k, lv_obj_t *parent, int size, uint32_t icon) {
  k.box = still_block(parent, theme::RAISED);
  lv_obj_set_style_radius(k.box, LV_RADIUS_CIRCLE, 0);
  k.label = ui::label(k.box, Font::ICON_SMALL, theme::INK);
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

// The deck: what the chosen speaker plays (its cover, two lines, previous, play or pause, next), the speakers as chips,
// the filters as chips with the page keys at their right, and a page of the library, one row each, a tap plays it.
// A tile too small for all that shows the row of what plays alone, and a tap beside its key opens the card with the
// whole deck. The tile and the card each hold one; the plugin hands each the answers of the app.
class Deck : public tessera::InPsram {
 public:
  Deck(MusicDeck *plugin, const Setup &setup, const char *entity) : plugin_(plugin), setup_(setup) {
    if (entity && *entity) keep(speakers_[count_++].entity, entity);
    for (int i = 0; i < SPEAKERS - 1 && count_ < SPEAKERS; ++i) {
      if (!setup_.extra[i][0] || (entity && !strcmp(setup_.extra[i], entity))) continue;
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
  }
  void tell(Handoff &h) const {
    h.setup = setup_;
    h.count = count_;
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
    key_ = std::max(ui::mm(8), ui::line_height(Font::ICON_SMALL) + ui::px(14));
    gap_ = ui::px(8);
    row_h_ = std::max(ui::mm(8), ui::line_height(Font::BODY_LARGE) + ui::px(12));
    row_gap_ = ui::px(4);
    now_h_ = std::max(key_, ui::line_height(Font::TITLE) + ui::line_height(Font::BODY) + ui::px(4));
    // The whole deck needs the row of what plays, the chips and two rows of the library. On a short glass the speakers
    // share the filters' row, one chip each that steps on, so the list keeps its room.
    const int list = 2 * (row_h_ + row_gap_);
    merged_ = count_ > 1 && h < now_h_ + gap_ + 2 * (key_ + gap_) + list;
    const int chips = (count_ > 1 && !merged_ ? key_ + gap_ : 0) + key_ + gap_;
    full_ = whole || (h >= now_h_ + gap_ + chips + list && w >= ui::mm(45));
    if (!full_) {
      make_small(parent);
      paint();
      return;
    }
    make_now(parent, now_h_, true);
    int y = now_h_ + gap_;
    if (count_ > 1) {
      speakers_y_ = y;
      for (int i = 0; i < count_; ++i) make_chip(speaker_chips_[i], parent);
      if (!merged_) y += key_ + gap_;
    }
    filters_y_ = y;
    for (int i = 0; i < setup_.filter_count; ++i) make_chip(filter_chips_[i], parent);
    make_key(left_, parent, key_, ICON_LEFT);
    make_key(right_, parent, key_, ICON_RIGHT);
    right_.place(w_ - key_, y, key_, key_);
    left_.place(w_ - 2 * key_ - gap_, y, key_, key_);
    y += key_ + gap_;
    list_y_ = y;
    rows_ = std::max(1, std::min(ROWS, (h_ - y + row_gap_) / (row_h_ + row_gap_)));
    for (int i = 0; i < rows_; ++i) {
      Part &r = rows_parts_[i];
      r.box = still_block(parent, theme::RAISED);
      r.label = ui::label(r.box, Font::BODY_LARGE, theme::INK);
      r.place(0, y + i * (row_h_ + row_gap_), w_, row_h_);
      lv_obj_set_width(r.label, w_ - 2 * ui::px(12));
      lv_obj_set_pos(r.label, ui::px(12), (row_h_ - ui::line_height(Font::BODY_LARGE)) / 2);
      r.show(false);
    }
    note_ = ui::text(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, w_, 2 * ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, 0, list_y_ + std::max(0, (h_ - list_y_ - 2 * ui::line_height(Font::BODY)) / 2));
    lay_chips();
    ask();
    paint();
  }

  void state(JsonObjectConst data) {
    if (!data["state"].isNull() && count_ && speakers_[0].option < 0) take(speakers_[0], data);
    JsonObjectConst entities = data["entities"].as<JsonObjectConst>();
    if (!entities.isNull())
      for (int i = 0; i < count_; ++i)
        if (speakers_[i].option >= 0 && !entities[SPEAKER_OPTIONS[speakers_[i].option]].isNull())
          take(speakers_[i], entities[SPEAKER_OPTIONS[speakers_[i].option]].as<JsonObjectConst>());
    lay_chips();
    paint();
  }

  void tick() {
    const uint32_t now = esphome::millis();
    // A started row goes back to the list's look once the speaker had its time to start.
    if (hint_at_ && now - hint_at_ > 5000) {
      hint_at_ = 0;
      paint_list();
    }
    if (sent_row_ >= 0 && now - sent_at_ > 10000) {
      sent_row_ = -1;
      paint();
    }
    if (!full_) return;
    // No answer within a while is no answer; a page that did not come is asked again now and then.
    if (asked_ && now - asked_at_ > 15000) {
      asked_ = 0;
      wait_ = Wait::NO_ANSWER;
      paint_list();
    }
    // Not sent (the screen not yet linked to the app, as just after it starts): again soon; a refusal or no answer
    // now and then.
    if (!asked_ && wait_ == Wait::OFFLINE && now - asked_at_ > 5000) ask();
    if (!asked_ && (wait_ == Wait::FAILED || wait_ == Wait::NO_ANSWER) && now - asked_at_ > 30000) ask();
  }

  // A tap on the deck; false when it hit none of its parts (a small tile then opens the card).
  bool tap(tessera::Point p) {
    const int slack = ui::px(4);
    Speaker &s = speakers_[at_];
    if (play_.hit(p, slack)) {
      if (s.entity[0]) tessera::wish(s.entity, tessera::Wish::PLAYING, playing(s) ? "0" : "1");
      return true;
    }
    if (next_.hit(p, slack)) {
      if (s.entity[0]) tessera::action(NEXT, s.entity);
      return true;
    }
    if (previous_.hit(p, slack)) {
      if (s.entity[0]) tessera::action(PREVIOUS, s.entity);
      return true;
    }
    if (!full_) return false;
    for (int i = 0; i < count_ && count_ > 1; ++i) {
      if (!speaker_chips_[i].hit(p, slack)) continue;
      at_ = all_speakers_ ? i : (at_ + 1) % count_;
      lay_chips();
      paint();
      return true;
    }
    for (int i = 0; i < setup_.filter_count; ++i) {
      if (!filter_chips_[i].hit(p, slack)) continue;
      const int next = all_filters_ ? i : (filter_ + 1) % setup_.filter_count;
      if (next != filter_ || !all_filters_) {
        filter_ = next;
        offset_ = 0;
        lay_chips();
        ask();
      }
      return true;
    }
    if (left_.hit(p, slack)) {
      if (offset_ > 0) {
        offset_ = std::max(0, offset_ - rows_);
        ask();
      }
      return true;
    }
    if (right_.hit(p, slack)) {
      if (more_) {
        offset_ += items_count_;
        ask();
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
    if (full_) {
      paint_chips();
      paint_list();
    }
  }

 private:
  enum class Wait : uint8_t { NONE, LOADING, EMPTY, FAILED, NO_ENTRY, NO_ANSWER, OFFLINE };
  struct Item {
    char name[52] = {};
    char uri[52] = {};
  };

  static void take(Speaker &s, JsonObjectConst e) {
    keep(s.state, e["state"] | "");
    keep(s.name, e["name"] | "");
    keep(s.title, e["attributes"]["media_title"] | "");
    keep(s.artist, e["attributes"]["media_artist"] | "");
  }
  static bool playing(const Speaker &s) { return !strcmp(s.state, "playing") || !strcmp(s.state, "buffering"); }
  static bool active(const Speaker &s) { return playing(s) || !strcmp(s.state, "paused"); }
  const char *name_of(const Speaker &s) const { return s.name[0] ? s.name : s.entity; }

  // The row of what plays, `h` high from the top: the cover (or a music icon), the title and the artist, and the keys
  // at the right. `three`: previous and next beside play, where they fit.
  void make_now(lv_obj_t *parent, int h, bool three) {
    const int text_min = ui::mm(14);
    int side = h;
    if (w_ < side + gap_ + key_ + text_min) side = 0;
    art(parent, 0, 0, side);
    int right = w_;
    const bool all = three && w_ - side - gap_ - text_min >= 3 * key_ + 2 * gap_;
    const int ky = (h - key_) / 2;
    if (all) {
      make_key(next_, parent, key_, ICON_NEXT);
      next_.place(right - key_, ky, key_, key_);
      right -= key_ + gap_;
    }
    make_key(play_, parent, key_, ICON_PLAY);
    play_.place(right - key_, ky, key_, key_);
    right -= key_ + gap_;
    if (all) {
      make_key(previous_, parent, key_, ICON_PREVIOUS);
      previous_.place(right - key_, ky, key_, key_);
      right -= key_ + gap_;
    }
    const int x = side ? side + gap_ : 0;
    if (right + gap_ - x < text_min) return;
    const int tw = right - x;
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY);
    title_ = ui::label(parent, Font::TITLE, theme::INK);
    lv_obj_set_width(title_, tw);
    lv_obj_set_pos(title_, x, (h - th - bh) / 2);
    artist_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(artist_, tw);
    lv_obj_set_pos(artist_, x, (h - th - bh) / 2 + th);
  }

  // A small tile: one lying down is the row of what plays over its height; a square or standing one is the cover
  // with the key under it, the two in the middle of the height (the key alone where no cover fits over it).
  void make_small(lv_obj_t *parent) {
    if (w_ * 2 > h_ * 3) {
      make_now(parent, h_, false);
      return;
    }
    int side = std::min(w_, h_ - key_ - gap_);
    if (side < ui::mm(8)) side = 0;
    const int top = (h_ - (side ? side + gap_ : 0) - key_) / 2;
    art(parent, (w_ - side) / 2, top, side);
    make_key(play_, parent, key_, ICON_PLAY);
    play_.place((w_ - key_) / 2, top + (side ? side + gap_ : 0), key_, key_);
  }

  // The square at the left of what plays: a music icon, and over it the cover where the screen shows pictures.
  void art(lv_obj_t *parent, int x, int y, int side) {
    if (side <= 0) return;
    art_ = still_block(parent, theme::TRACK);
    lv_obj_set_size(art_, side, side);
    lv_obj_set_pos(art_, x, y);
    art_icon_ = ui::label(art_, Font::ICON, theme::MUTED);
    ui::set_text(art_icon_, ui::icon(ICON_MUSIC));
    lv_obj_center(art_icon_);
    if (!pictures_) return;
    // No block of its own while the cover comes: the icon shows through until it is there.
    cover_ = ui::picture(parent, tessera::Placeholder::NONE);
    lv_obj_set_size(cover_, side, side);
    lv_obj_set_pos(cover_, x, y);
  }

  // The chips of one row from `x` up to `room` wide: every word when they all fit, else one chip, the chosen one,
  // which a tap steps on. True when they all fit.
  bool lay_row(Part *parts, int n, const std::string *words, int chosen, int x, int y, int room) {
    const int pad = ui::px(12);
    auto width = [&](const std::string &word) { return ui::text_width(word, Font::BODY) + 2 * pad; };
    int need = 0;
    for (int i = 0; i < n; ++i) need += width(words[i]) + (i ? gap_ : 0);
    const bool all = need <= room;
    for (int i = 0; i < n; ++i) {
      Part &c = parts[i];
      if (!all && i) {
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
    // The filters' row ends where the page keys start; on a short glass the speakers take its first half.
    const int room = w_ - 2 * key_ - 2 * gap_;
    const int half = merged_ ? (room - gap_) / 2 : 0;
    if (count_ > 1) {
      std::string words[SPEAKERS];
      for (int i = 0; i < count_; ++i) words[i] = name_of(speakers_[i]);
      all_speakers_ = lay_row(speaker_chips_, count_, words, at_, 0, speakers_y_, merged_ ? half : w_);
    }
    std::string words[FILTERS];
    for (int i = 0; i < setup_.filter_count; ++i) words[i] = plugin_->text(FILTER[setup_.filters[i]].word);
    all_filters_ = lay_row(filter_chips_, setup_.filter_count, words, filter_, merged_ ? half + gap_ : 0, filters_y_,
                           merged_ ? room - half - gap_ : room);
  }

  void ask() {
    sent_row_ = -1;
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
    wait_ = asked_ ? Wait::LOADING : Wait::OFFLINE;
    paint_list();
  }

  void play(int i) {
    const Speaker &s = speakers_[at_];
    if (!s.entity[0]) {
      // Nowhere to play: say where a player is chosen, for a few seconds.
      hint_at_ = esphome::millis();
      paint_list();
      return;
    }
    const Item &it = items_[i];
    JsonDocument d;
    d["media_id"] = !it.uri[0] || maybe_cut(it.uri) ? it.name : it.uri;
    d["media_type"] = setup_.kind;
    d["enqueue"] = setup_.enqueue;
    if (tessera::action(PLAY, s.entity, d)) {
      sent_row_ = i;
      sent_at_ = esphome::millis();
    }
    paint();
  }

  void paint_now() {
    const Speaker *s = count_ ? &speakers_[at_] : nullptr;
    const bool on = s && active(*s);
    if (title_) {
      ui::set_color(title_, theme::INK);
      ui::set_color(artist_, theme::MUTED);
      std::string top, under;
      if (!s) {
        top = plugin_->text("no_player");
      } else if (sent_row_ >= 0) {
        top = items_[sent_row_].name;
        under = tessera::fill(plugin_->text("starting"), "player", name_of(*s));
      } else if (on && s->title[0]) {
        top = s->title;
        under = s->artist[0] ? s->artist : name_of(*s);
      } else {
        top = plugin_->text("nothing");
        under = !strcmp(s->state, "off") ? plugin_->text("off") : name_of(*s);
      }
      ui::set_text(title_, top);
      ui::set_text(artist_, under);
    }
    if (art_) {
      ui::set_fill(art_, Tone::NORMAL, theme::TRACK);
      ui::set_color(art_icon_, theme::MUTED);
    }
    // The cover of the tile's own player only: the app paints Home Assistant's pictures of the tile's own entity. With no
    // address the place shows nothing, and the music icon under it shows through.
    if (cover_) {
      const bool own_cover = s && s->option < 0 && on;
      ui::set_picture(cover_, own_cover ? std::string("cover:") + s->entity : "", tessera::Fit::COVER);
    }
    if (play_.box) {
      ui::set_text(play_.label, ui::icon(s && playing(*s) ? ICON_PAUSE : ICON_PLAY));
      ui::set_fill(play_.box, Tone::ACCENT, theme::ACCENT);
      ui::set_color(play_.label, theme::ON_ACCENT);
    }
    for (Part *k : {&previous_, &next_}) {
      if (!k->box) continue;
      ui::set_fill(k->box, Tone::NORMAL, surface_);
      ui::set_color(k->label, theme::INK);
    }
  }

  void paint_chip(Part &c, bool chosen) {
    if (!c.box) return;
    ui::set_fill(c.box, chosen ? Tone::ACCENT : Tone::NORMAL, surface_);
    ui::set_color(c.label, chosen ? theme::ON_ACCENT : theme::INK);
  }

  void paint_chips() {
    for (int i = 0; i < count_ && count_ > 1; ++i) paint_chip(speaker_chips_[i], !all_speakers_ || i == at_);
    for (int i = 0; i < setup_.filter_count; ++i) paint_chip(filter_chips_[i], !all_filters_ || i == filter_);
  }

  void paint_list() {
    if (!full_) return;
    for (int i = 0; i < rows_; ++i) {
      Part &r = rows_parts_[i];
      const bool there = wait_ == Wait::NONE && !hint_at_ && i < items_count_;
      r.show(there);
      if (!there) continue;
      ui::set_text(r.label, items_[i].name);
      const bool sent = i == sent_row_;
      ui::set_fill(r.box, sent ? Tone::ACCENT : Tone::NORMAL, surface_);
      ui::set_color(r.label, sent ? theme::ON_ACCENT : theme::INK);
    }
    const char *why = wait_ == Wait::LOADING ? "loading" : wait_ == Wait::EMPTY ? "empty"
                      : wait_ == Wait::FAILED ? "failed" : wait_ == Wait::NO_ENTRY ? "no_entry"
                      : wait_ == Wait::NO_ANSWER ? "no_answer" : wait_ == Wait::OFFLINE ? "offline" : nullptr;
    std::string note = why ? plugin_->text(why) : "";
    if (wait_ == Wait::FAILED) note = tessera::fill(note.c_str(), "reason", error_[0] ? error_ : "?");
    if (hint_at_) note = plugin_->text("pick_player");
    ui::set_text(note_, note);
    ui::set_color(note_, theme::MUTED);
    // The page keys: lit where they lead somewhere.
    ui::set_fill(left_.box, Tone::NORMAL, surface_);
    ui::set_fill(right_.box, Tone::NORMAL, surface_);
    ui::set_color(left_.label, offset_ > 0 ? theme::INK : theme::SUBTLE);
    ui::set_color(right_.label, more_ ? theme::INK : theme::SUBTLE);
    paint_now();
  }

  MusicDeck *plugin_;
  Setup setup_;
  Speaker speakers_[SPEAKERS];
  Item items_[ROWS];
  int count_ = 0, at_ = 0, filter_ = 0, offset_ = 0, items_count_ = 0, rows_ = 0, sent_row_ = -1;
  int w_ = 0, h_ = 0, key_ = 0, gap_ = 0, row_h_ = 0, row_gap_ = 0, now_h_ = 0;
  int speakers_y_ = 0, filters_y_ = 0, list_y_ = 0;
  bool full_ = false, merged_ = false, pictures_ = false, more_ = false, all_speakers_ = true, all_filters_ = true;
  Wait wait_ = Wait::NONE;
  theme::Role surface_ = theme::RAISED;
  uint32_t asked_ = 0, asked_at_ = 0, sent_at_ = 0, hint_at_ = 0;
  char error_[124] = {};
  Part play_, next_, previous_, left_, right_;
  Part speaker_chips_[SPEAKERS], filter_chips_[FILTERS], rows_parts_[ROWS];
  lv_obj_t *title_{}, *artist_{}, *art_{}, *art_icon_{}, *cover_{}, *note_{};
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
  void on_tick(uint32_t) override { deck_->tick(); }
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
  void on_tick(uint32_t) override { deck_->tick(); }
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

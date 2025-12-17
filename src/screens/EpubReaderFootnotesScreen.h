#pragma once
#include "Screen.h"
#include "../../lib/Epub/Epub/FootnoteEntry.h"
#include <functional>
#include <memory>
#include <cstring>

class FootnotesData {
private:
  FootnoteEntry entries[32];
  int count;

public:
  FootnotesData() : count(0) {}

  void addFootnote(const char* number, const char* href) {
    if (count < 32) {
      strncpy(entries[count].number, number, 2);
      entries[count].number[2] = '\0';
      strncpy(entries[count].href, href, 63);
      entries[count].href[63] = '\0';
      count++;
    }
  }

  void clear() {
    count = 0;
  }

  int getCount() const {
    return count;
  }

  const FootnoteEntry* getEntry(int index) const {
    if (index >= 0 && index < count) {
      return &entries[index];
    }
    return nullptr;
  }
};

class EpubReaderFootnotesScreen final : public Screen {
  const FootnotesData& footnotes;
  const std::function<void()> onGoBack;
  const std::function<void(const char*)> onSelectFootnote;
  int selectedIndex;

public:
  EpubReaderFootnotesScreen(
      GfxRenderer& renderer,
      InputManager& inputManager,
      const FootnotesData& footnotes,
      const std::function<void()>& onGoBack,
      const std::function<void(const char*)>& onSelectFootnote)
      : Screen(renderer, inputManager),
        footnotes(footnotes),
        onGoBack(onGoBack),
        onSelectFootnote(onSelectFootnote),
        selectedIndex(0) {}

  void onEnter() override;
  void onExit() override;
  void handleInput() override;

private:
  void render();
};
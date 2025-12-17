#include "EpubReaderFootnotesScreen.h"
#include "config.h"
#include <GfxRenderer.h>

void EpubReaderFootnotesScreen::onEnter() {
  selectedIndex = 0;
  render();
}

void EpubReaderFootnotesScreen::onExit() {
  // Nothing to clean up
}

void EpubReaderFootnotesScreen::handleInput() {
  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoBack();
    return;
  }

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    const FootnoteEntry* entry = footnotes.getEntry(selectedIndex);
    if (entry) {
      Serial.printf("[%lu] [FNS] Selected footnote: %s -> %s\n",
                    millis(), entry->number, entry->href);

      // Appeler le callback - EpubReaderScreen gère la navigation
      onSelectFootnote(entry->href);
    }
    return;
  }

  bool needsRedraw = false;

  if (inputManager.wasPressed(InputManager::BTN_UP)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      needsRedraw = true;
    }
  }

  if (inputManager.wasPressed(InputManager::BTN_DOWN)) {
    if (selectedIndex < footnotes.getCount() - 1) {
      selectedIndex++;
      needsRedraw = true;
    }
  }

  if (needsRedraw) {
    render();
  }
}

void EpubReaderFootnotesScreen::render() {
  renderer.clearScreen();

  constexpr int startY = 50;
  constexpr int lineHeight = 40;
  constexpr int marginLeft = 20;

  // Title
  renderer.drawText(READER_FONT_ID, marginLeft, 20, "Footnotes", BOLD);

  if (footnotes.getCount() == 0) {
    renderer.drawText(SMALL_FONT_ID, marginLeft, startY + 20, "No footnotes on this page");
    renderer.displayBuffer();
    return;
  }

  // Display footnotes
  for (int i = 0; i < footnotes.getCount(); i++) {
    const FootnoteEntry* entry = footnotes.getEntry(i);
    if (!entry) continue;

    const int y = startY + i * lineHeight;

    // Draw selection indicator (arrow)
    if (i == selectedIndex) {
      renderer.drawText(READER_FONT_ID, marginLeft - 10, y, ">", BOLD);
      renderer.drawText(READER_FONT_ID, marginLeft + 10, y, entry->number, BOLD);
    } else {
      renderer.drawText(READER_FONT_ID, marginLeft + 10, y, entry->number);
    }
  }

  // Instructions at bottom
  renderer.drawText(SMALL_FONT_ID, marginLeft,
                   GfxRenderer::getScreenHeight() - 40,
                   "UP/DOWN: Select  CONFIRM: Go to footnote  BACK: Return");

  renderer.displayBuffer();
}
//
// Created by jlaunay on 13/12/2025.
//

#ifndef CROSSPOINT_READER_EPUBREADERMENUSCREEN_H
#define CROSSPOINT_READER_EPUBREADERMENUSCREEN_H
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "Screen.h"

class EpubReaderMenuScreen final : public Screen {
public:
  enum MenuOption {
    CHAPTERS,
    FOOTNOTES
  };

private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(MenuOption option)> onSelectOption;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

public:
  explicit EpubReaderMenuScreen(GfxRenderer& renderer, InputManager& inputManager,
                                const std::function<void()>& onGoBack,
                                const std::function<void(MenuOption option)>& onSelectOption)
      : Screen(renderer, inputManager),
        onGoBack(onGoBack),
        onSelectOption(onSelectOption) {}

  void onEnter() override;
  void onExit() override;
  void handleInput() override;
};
#endif  // CROSSPOINT_READER_EPUBREADERMENUSCREEN_H

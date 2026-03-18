#ifndef EDITOR_TYPES_H
#define EDITOR_TYPES_H

#include "arena.h"

/* Panel IDs for docking system */
typedef enum {
    PANEL_GAME,
    PANEL_EDITOR,
    PANEL_SCENE_TREE,
    PANEL_INSPECTOR,
    PANEL_OUTLINE,
    PANEL_ASSETS,
    PANEL_COUNT
} PanelId;

#ifndef MENU_BAR_HEIGHT
#define MENU_BAR_HEIGHT 28
#endif

#endif /* EDITOR_TYPES_H */

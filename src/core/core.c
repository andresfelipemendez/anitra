#include "core.h"
#include "loadlibrary.h"
#include <externals.h>
#include <project.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#endif


static volatile int reloadFlag = 0;
static volatile int editorReloadFlag = 0;
static volatile int shutdownRequested = 0;
static void *engine_lib = NULL;
static void *editor_lib = NULL;

#ifdef _WIN32
static HANDLE hEngineThread = NULL;
static HANDLE hEditorThread = NULL;

static DWORD WINAPI waitforreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for engine reload signal...\n");
  while (!shutdownRequested) {
    DWORD result = WaitForSingleObject(hEvent, 100);
    if (result == WAIT_OBJECT_0 && !shutdownRequested) {
      printf("Hot reload signal received (engine)\n");
      reloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}

static DWORD WINAPI waitforeditorreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for editor reload signal...\n");
  while (!shutdownRequested) {
    DWORD result = WaitForSingleObject(hEvent, 100);
    if (result == WAIT_OBJECT_0 && !shutdownRequested) {
      printf("Hot reload signal received (editor)\n");
      editorReloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}
#endif

EXPORT int init_core(const char *project_path) {
  printf("Core initialized\n");

#ifdef _WIN32
  HANDLE hEngineEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
  HANDLE hEditorEvent;

  if (hEngineEvent == NULL) {
    printf("CreateEvent (engine) failed (%lu)\n", GetLastError());
  }

  hEditorEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EDITOR_EVENT_NAME);
  if (hEditorEvent == NULL) {
    printf("CreateEvent (editor) failed (%lu)\n", GetLastError());
  }
#endif

  {
    memory g = {0};
    int reload;

    /* Set default asset paths */
    g.game.default_model_path = "assets/models/Knight.glb";
    g.game.default_animation_path = "assets/animations/Rig_Medium_General.glb";
    g.game.texture_player = "assets/char_spritesheet.png";
    g.game.texture_tiles = "assets/Dungeon_Tileset.png";
    g.game.texture_slime = "assets/pinkslime_spritesheet.png";
    g.game.texture_health_bar = "assets/health_bar_hud.png";
    g.game.texture_health_fill = "assets/health_hud.png";
    g.game.font_editor = "assets/fonts/SourceCodePro-Regular.ttf";
    g.game.shader_sprite_vs = "assets/shaders/compiled/sprite_vs.spv";
    g.game.shader_sprite_fs = "assets/shaders/compiled/sprite_fs.spv";
    g.game.shader_debug_lines_vs = "assets/shaders/compiled/debug_lines_vs.spv";
    g.game.shader_debug_lines_fs = "assets/shaders/compiled/debug_lines_fs.spv";
    g.game.shader_ui_rect_vs = "assets/shaders/compiled/ui_rect_vs.spv";
    g.game.shader_ui_rect_fs = "assets/shaders/compiled/ui_rect_fs.spv";
    g.game.shader_font_vs = "assets/shaders/compiled/font_vs.spv";
    g.game.shader_font_fs = "assets/shaders/compiled/font_fs.spv";
    g.game.shader_mesh_vs = "assets/shaders/compiled/mesh_vs.spv";
    g.game.shader_mesh_fs = "assets/shaders/compiled/mesh_fs.spv";
    g.game.shader_composite_vs = "assets/shaders/compiled/composite_vs.spv";
    g.game.shader_composite_fs = "assets/shaders/compiled/composite_fs.spv";

    /* Load project file if provided */
    if (project_path) {
        static project_data project;
        if (project_load(project_path, &project) == 0) {
            /* Override camera from project */
            if (project.has_camera) {
                g.game.mesh3d.camera_eye = VEC3(project.camera.eye[0],
                    project.camera.eye[1], project.camera.eye[2]);
                g.game.mesh3d.camera_target = VEC3(project.camera.target[0],
                    project.camera.target[1], project.camera.target[2]);
                g.game.mesh3d.camera_up = VEC3(project.camera.up[0],
                    project.camera.up[1], project.camera.up[2]);
                g.game.mesh3d.camera_set_by_project = 1;
            }

            /* Resolve player model path from asset registry */
            if (project.entity_count > 0) {
                int pi;
                project_entity *player = &project.entities[0];
                for (pi = 0; pi < project.model_count; pi++) {
                    if (strcmp(project.model_keys[pi], player->model) == 0) {
                        g.game.default_model_path = project.model_paths[pi];
                        break;
                    }
                }
                for (pi = 0; pi < project.animation_count; pi++) {
                    if (strcmp(project.animation_keys[pi], player->animations) == 0) {
                        g.game.default_animation_path = project.animation_paths[pi];
                        break;
                    }
                }
            }

            /* Override sprite/texture paths from project assets */
            {
                int si;
                for (si = 0; si < project.sprite_count; si++) {
                    if (strcmp(project.sprite_keys[si], "player_sheet") == 0)
                        g.game.texture_player = project.sprite_paths[si];
                    else if (strcmp(project.sprite_keys[si], "slime_sheet") == 0)
                        g.game.texture_slime = project.sprite_paths[si];
                    else if (strcmp(project.sprite_keys[si], "health_bar") == 0)
                        g.game.texture_health_bar = project.sprite_paths[si];
                    else if (strcmp(project.sprite_keys[si], "health_fill") == 0)
                        g.game.texture_health_fill = project.sprite_paths[si];
                }
            }

            /* Populate lighting from project */
            if (project.has_lighting) {
                int li;
                g.game.lighting.ambient = VEC3(
                    project.lighting.ambient[0],
                    project.lighting.ambient[1],
                    project.lighting.ambient[2]);
                g.game.lighting.light_count = project.lighting.point_light_count;
                for (li = 0; li < project.lighting.point_light_count; li++) {
                    project_point_light *src = &project.lighting.point_lights[li];
                    g.game.lighting.lights[li].position = VEC3(
                        src->position[0], src->position[1], src->position[2]);
                    g.game.lighting.lights[li].color = VEC3(
                        src->color[0], src->color[1], src->color[2]);
                    g.game.lighting.lights[li].intensity = src->intensity;
                    g.game.lighting.lights[li].radius = src->radius;
                }
            }
        }
    }

/* Load engine DLL (copy first so engine.dll stays unlocked) */
    copylibrary("engine", "engine_copy");
    engine_lib = loadlibrary("engine_copy");
    if (!engine_lib) {
      fprintf(stderr, "Failed to load engine_copy.dll\n");
      return 1;
    }
    
    engine_init_fn init_e = (engine_init_fn)getfunction(engine_lib, "init_engine");
    engine_destroy_fn destroy_e = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
    engine_update_fn update_e = (engine_update_fn)getfunction(engine_lib, "update_engine");
    
    if (!init_e || !destroy_e || !update_e) {
      fprintf(stderr, "Failed to get engine functions\n");
      unloadlibrary(engine_lib);
      return 1;
    }
    
    assign_init(init_e);
    assign_destroy(destroy_e);
    assign_update(update_e);

    /* Load editor DLL (copy first so editor.dll stays unlocked) */
    copylibrary("editor", "editor_copy");
    editor_lib = loadlibrary("editor_copy");
    if (editor_lib) {
      editor_init_fn init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
      editor_destroy_fn destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
      editor_update_fn update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
      editor_handle_event_fn handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");

      if (!init_ed || !destroy_ed || !update_ed) {
        fprintf(stderr, "Warning: Editor functions not found - editor disabled\n");
        unloadlibrary(editor_lib);
        editor_lib = NULL;
      } else {
        assign_editor_init(init_ed);
        assign_editor_destroy(destroy_ed);
        assign_editor_update(update_ed);
        assign_editor_handle_event(handle_ev);
      }
    }
    init_externals(&g);
    init_engine(&g);
    init_editor(&g);

#ifdef _WIN32
    /* Reset state for fresh load */
    shutdownRequested = 0;
    reloadFlag = 0;
    editorReloadFlag = 0;

    hEngineThread = CreateThread(NULL, 0, waitforreloadsignal, hEngineEvent, 0, NULL);
    hEditorThread = CreateThread(NULL, 0, waitforeditorreloadsignal, hEditorEvent, 0, NULL);
#endif

    reload = begin_game_loop(&g);

#ifdef _WIN32
    /* Stop reload threads FIRST — they run code from this DLL,
       so they must exit before we unload anything. */
    shutdownRequested = 1;
    if (hEngineThread) {
      WaitForSingleObject(hEngineThread, 500);
      CloseHandle(hEngineThread);
      hEngineThread = NULL;
    }
    if (hEditorThread) {
      WaitForSingleObject(hEditorThread, 500);
      CloseHandle(hEditorThread);
      hEditorThread = NULL;
    }

    /* Close named events */
    if (hEngineEvent) CloseHandle(hEngineEvent);
    if (hEditorEvent) CloseHandle(hEditorEvent);
#endif

    /* Teardown */
    destroy_editor(&g);
    destroy_engine(&g);
    end_externals(&g);

    unloadlibrary(engine_lib);
    unloadlibrary(editor_lib);
    engine_lib = NULL;
    editor_lib = NULL;

    return reload;
  }
}

int begin_game_loop(memory *g) {
#ifdef _WIN32
  HANDLE hCoreEvent = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE,
                                FALSE, HOTRELOAD_CORE_EVENT_NAME);
#endif

  while (g->game.play) {
#ifdef _WIN32
if (reloadFlag) {
      reloadFlag = 0;
      
      /* Verify new DLL was loaded */
      if (!engine_lib) {
        fprintf(stderr, "Engine library is NULL - cannot reload\n");
        continue;
      }
      
      destroy_engine(g);
      printf("Reloading engine...\n");

      unloadlibrary(engine_lib);
      copylibrary("engine", "engine_copy");
      engine_lib = loadlibrary("engine_copy");
      if (!engine_lib) {
        fprintf(stderr, "Failed to reload engine_copy.dll\n");
        continue;
      }
      
engine_init_fn new_init = (engine_init_fn)getfunction(engine_lib, "init_engine");
      engine_destroy_fn new_destroy = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
      engine_update_fn new_update = (engine_update_fn)getfunction(engine_lib, "update_engine");

      if (!new_init || !new_destroy || !new_update) {
        fprintf(stderr, "Failed to get engine functions after reload\n");
        unloadlibrary(engine_lib);
        continue;
      }

      assign_init(new_init);
      assign_destroy(new_destroy);
      assign_update(new_update);
      init_engine(g);
    }

    if (editorReloadFlag) {
      editorReloadFlag = 0;
      
      /* Verify new DLL was loaded */
      if (!editor_lib) {
        fprintf(stderr, "Editor library is NULL - cannot reload\n");
        continue;
      }
      
      destroy_editor(g);
      printf("Reloading editor...\n");

      unloadlibrary(editor_lib);
      copylibrary("editor", "editor_copy");
      editor_lib = loadlibrary("editor_copy");
      if (!editor_lib) {
        fprintf(stderr, "Failed to reload editor_copy.dll\n");
        continue;
      }

      editor_init_fn new_init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
      editor_destroy_fn new_destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
      editor_update_fn new_update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
      editor_handle_event_fn new_handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");

      if (!new_init_ed || !new_destroy_ed || !new_update_ed) {
        fprintf(stderr, "Failed to get editor functions after reload\n");
        unloadlibrary(editor_lib);
        editor_lib = NULL;
        continue;
      }

      assign_editor_init(new_init_ed);
      assign_editor_destroy(new_destroy_ed);
      assign_editor_update(new_update_ed);
      assign_editor_handle_event(new_handle_ev);
      init_editor(g);
    }
    /* Core reload: poll with 0ms timeout (non-blocking) */
    if (hCoreEvent && WaitForSingleObject(hCoreEvent, 0) == WAIT_OBJECT_0) {
      printf("Core reload signal received — tearing down...\n");
      ResetEvent(hCoreEvent);
      CloseHandle(hCoreEvent);
      return 1; /* signal reload to init_core */
    }
#endif
    update_externals(g);
  }

#ifdef _WIN32
  if (hCoreEvent) CloseHandle(hCoreEvent);
#endif
  return 0; /* normal exit */
}

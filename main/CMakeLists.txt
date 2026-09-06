idf_component_register(
    SRCS "main.c"
         "todo_app.c"
         "todo_sync.c"
         "todo_config.c"
         "todo_model.c"
         "todo_dotfont.c"
         "todo_font_cjk_12.c"
         "todo_text_assets.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi esp_event esp_http_client esp-tls esp_http_server nvs_flash json esp_timer
)

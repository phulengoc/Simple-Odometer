idf_component_register(
  SRCS "user_app.c"
  PRIV_REQUIRES touch_bsp sd_card_bsp ui_bsp
#  REQUIRES ui_bsp
  INCLUDE_DIRS "./")

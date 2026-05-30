#include <stdio.h>
#include <dirent.h>
#include "user_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "gui_guider.h"
#include "sd_card_bsp.h"
lv_ui guider_ui;
EventGroupHandle_t TaskEven;
static void screen_event_cb (lv_event_t *e);
void FileSCAN_example(void *arg);
void lv_clear_list(lv_obj_t *obj,uint8_t value);
void setHIDDEN(uint8_t value);
void user_Init(void)
{
  TaskEven = xEventGroupCreate();
  SD_card_Init();
  setup_ui(&guider_ui);
  setHIDDEN(1);
  lv_clear_list(guider_ui.screen_list_1,20);
  lv_obj_add_event_cb(guider_ui.screen_btn_2, screen_event_cb, LV_EVENT_ALL, &guider_ui);    
  lv_obj_add_event_cb(guider_ui.screen_btn_1, screen_event_cb, LV_EVENT_ALL, &guider_ui);   
  lv_obj_add_event_cb(guider_ui.screen_btn_3, screen_event_cb, LV_EVENT_ALL, &guider_ui);    
  for(uint8_t i = 0; i<20; i++)
  {
    lv_obj_t *imt = lv_obj_get_child(guider_ui.screen_list_1,i);
    lv_obj_add_event_cb(imt, screen_event_cb, LV_EVENT_ALL, &guider_ui);    
  }
  xTaskCreate(FileSCAN_example, "FileSCAN_example", 3000, &guider_ui , 2, NULL);
}
void lv_clear_list(lv_obj_t *obj,uint8_t value) 
{
	for(signed char i = value-1; i>=0; i--)
	{
		lv_obj_t *imte = lv_obj_get_child(obj,i);
		lv_obj_add_flag(imte,LV_OBJ_FLAG_HIDDEN);
		vTaskDelay(pdMS_TO_TICKS(20));
	}
  vTaskDelay(pdMS_TO_TICKS(20));
  lv_obj_invalidate(obj); //Redraw the next cycle
}
void setHIDDEN(uint8_t value)
{
  switch (value)
  {
  case 1:
    lv_obj_clear_flag(guider_ui.screen_cont_1,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(guider_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(guider_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    break;
  case 2:
    lv_obj_clear_flag(guider_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(guider_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(guider_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    break;
  case 3:
    lv_obj_clear_flag(guider_ui.screen_cont_3,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(guider_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(guider_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
    break;
  default:
    break;
  }
}
static void screen_event_cb (lv_event_t *e)
{
  static char IMG_NAME[25] = {0};
  static char IMG_NAME_Host[10] = {0};
  static char IMG_NAME_Tial[5] = {0};
  lv_event_code_t code = lv_event_get_code(e);
  lv_ui *ui = (lv_ui *)e->user_data;
  lv_obj_t * module = e->current_target;
  switch (code)
  {
    case LV_EVENT_CLICKED:
    {
      if(module == ui->screen_btn_1)
      {
        xEventGroupSetBits( TaskEven,(0x01<<0) );
      }
      else if(module == ui->screen_btn_2)
      {
        setHIDDEN(1);
      }
      else if(module == ui->screen_btn_3)
      {
        setHIDDEN(2);
      }
      else 
      {
        //uint8_t vaOBJ = lv_obj_get_index(module);
        lv_obj_t *label = lv_obj_get_child(module,1);
        sscanf(lv_label_get_text(label),"%[^.].%s",IMG_NAME_Host,IMG_NAME_Tial);
        if(!strcmp(IMG_NAME_Tial,"PNG"))
        {
          sprintf(IMG_NAME,"S:/Test/%s.png",IMG_NAME_Host);
        }
        else if(!strcmp(IMG_NAME_Tial,"BMP"))
        {
          sprintf(IMG_NAME,"S:/Test/%s.bmp",IMG_NAME_Host);
        }
        else
        {
          sprintf(IMG_NAME,"S:/Test/%s.jpg",IMG_NAME_Host);
        }
        //printf("%s\n",IMG_NAME);
        lv_img_set_src(ui->screen_img_1, IMG_NAME);
        setHIDDEN(3);
      }
      break;
    }
    default:
      break;
  }
}
void FileSCAN_example(void *arg)
{
  lv_ui *obj = (lv_ui *)arg;
  uint16_t rec = 0;
  lv_obj_t *imte = NULL;
  lv_obj_t *label = NULL;
  for(;;)
  {
    EventBits_t even = xEventGroupWaitBits(TaskEven,(0x01<<0),pdTRUE,pdFALSE,1000);
    if( (even>>0) & 0x01 )
    {
      lv_clear_list(obj->screen_list_1,rec);
      rec = 0;
      for(;;)
      {
        DIR *dir = opendir("/sd_card/Test");
        if (dir == NULL)
        {
          printf("Error opening directory %s\n", "/sd_card/Test");
          break;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
          printf("File: %s\n", entry->d_name);
          imte = lv_obj_get_child(obj->screen_list_1,rec);  //Get the first
          if (imte != NULL)
			    {
			    	label = lv_obj_get_child(imte,1);
			    	if(label != NULL)
            {
    	    		lv_label_set_text(label,entry->d_name);
			    		lv_obj_clear_flag(imte,LV_OBJ_FLAG_HIDDEN);    
			    	}
			    }
          imte = NULL;
			    label = NULL;
          rec++;
          if(rec == 20)
          break;
        }
        closedir(dir);
        break;
      }
      lv_obj_clear_flag(obj->screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
      lv_obj_add_flag(obj->screen_cont_1, LV_OBJ_FLAG_HIDDEN);
    }  
  }
}
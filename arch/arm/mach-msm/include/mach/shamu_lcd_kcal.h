#ifndef __HTC_LCD_KCAL_H
#define __HTC_LCD_KCAL_H
struct kcal_data {
	int red;
	int green;
	int blue;
};
struct kcal_platform_data {
	int (*set_values) (int r, int g, int b);
	int (*get_values) (int *r, int *g, int *b);
	int (*refresh_display) (void);
};
void __init shamu_add_lcd_kcal_devices(void);
#endif

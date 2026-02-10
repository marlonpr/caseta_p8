idf_component_register(
	SRCS "main.c"
	INCLUDE_DIRS "."
	REQUIRES led_panel lora02 DS18B20 DS3231
)
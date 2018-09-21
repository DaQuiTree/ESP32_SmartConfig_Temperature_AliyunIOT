/* DHT11 temperature sensor library
   Usage:
   		Set DHT PIN using  setDHTPin(pin) command
   		getFtemp(); this returns temperature in F
   Sam Johnston 
   October 2016
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#ifndef DHT11_H_  
#define DHT11_H_

#define DHT_TIMEOUT_ERROR -2
#define DHT_CHECKSUM_ERROR -1
#define DHT_OKAY  0

typedef struct{
	int temp;
	int Ftemp;
	int humidity;
} envT;

// function prototypes

//Start by using this function
void setDHTPin(int PIN);

//call each function for live temperature updates
//if you only need one measurements use these functions
int getFtemp();
int getTemp();
int getHumidity();
int getEnvData(envT* edp);

#endif

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ff.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* DUAL_CORE_BOOT_SYNC_SEQUENCE: Define for dual core boot synchronization    */
/*                             demonstration code based on hardware semaphore */
/* This define is present in both CM7/CM4 projects                            */
/* To comment when developping/debugging on a single core                     */
#define DUAL_CORE_BOOT_SYNC_SEQUENCE

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

SD_HandleTypeDef hsd1;

/* USER CODE BEGIN PV */
#define BUFFER_SIZE_BYTES     (32 * 1024)

#define SD_BLOCK_SIZE         512
#define SD_BLOCK_COUNT        (BUFFER_SIZE_BYTES / SD_BLOCK_SIZE)

#define START_SECTOR          0x1000

#define CHUNKS_PER_FILE       64    // 生成 約 1.7MB 大小的檔案，檔案內容資料有 32768 筆資料

#define MAX_FILES_TO_GENERATE 5   // 控制： 最多生成幾個檔案 方便測試。  5 -> 產生5個檔案

#define PAYLOAD_SIZE        54

#define PAYLOAD_BUFFER_SIZE \
    (RECORD_COUNT * PAYLOAD_SIZE)

#define RECORD_COUNT   (BUFFER_SIZE_BYTES / sizeof(log_record_t))

typedef struct
{
    uint8_t payload[PAYLOAD_SIZE];

    uint16_t tail;

    uint32_t record_id;

    uint32_t file_id;

} log_record_t;

FIL MyFile;

__attribute__((section(".RAM_D1")))
__attribute__((aligned(32)))
log_record_t read_buffer[RECORD_COUNT];

/*
--------------------------------------------------
高速 FATFS write buffer
512 records × 54 bytes
= 27648 bytes
--------------------------------------------------
*/
__attribute__((section(".RAM_D1")))
__attribute__((aligned(32)))
uint8_t payload_buffer[PAYLOAD_BUFFER_SIZE];

uint32_t current_sector;

volatile uint8_t sd_read_done = 0;
volatile uint8_t sd_read_error = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_SDMMC1_SD_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*
每個 chunk 檢查： 512 records 是否： 全部都是合法 record 
只要有：  tail != 0xAA55
代表： segment 結束
這樣：
  可以自動停止
  不必固定 64
  不怕不完整資料
  不怕 SD card 中斷
  不怕 reboot
*/

uint8_t ValidateChunk(void)
{
    uint32_t i;

    for(i = 0; i < RECORD_COUNT; i++)
    {
        if(read_buffer[i].tail != 0xAA55)
        {
            return 0;
        }
    }

    return 1;
}


void GenerateFatFsFile(uint32_t file_id)
{
    UINT bw;

    char filename[64];

    uint32_t chunk;
	
	uint32_t i;

    uint32_t payload_offset;

    current_sector =
        START_SECTOR +
        (file_id * CHUNKS_PER_FILE * SD_BLOCK_COUNT);

    sprintf(filename,
            "LOG%04lu.TXT",
            file_id);

    
    //PA6 HIGH 開始量測：  Raw Data -> FATFS 檔案生成總時間    
    HAL_GPIO_WritePin(GPIOA,
                      GPIO_PIN_6,
                      GPIO_PIN_SET);

    

    // 建立 FATFS 檔案
    if(f_open(&MyFile,
              filename,
              FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    {
        BSP_LED_On(LED_RED);
        while(1);
    }

    // 開始讀取 Raw Data
    for(chunk = 0; chunk < CHUNKS_PER_FILE; chunk++)
    {
        //DMA Read 32KB
        sd_read_done = 0;
        sd_read_error = 0;

        if(HAL_SD_ReadBlocks_DMA(&hsd1,
                                (uint8_t*)read_buffer,
                                current_sector,
                                SD_BLOCK_COUNT) != HAL_OK)
        {
            BSP_LED_On(LED_RED);
            while(1);
        }

        //等待 DMA 完成
        while(sd_read_done == 0)
        {
            if(sd_read_error)
            {
                BSP_LED_On(LED_RED);

                while(1);
            }
            BSP_LED_On(LED_YELLOW);
        }
        BSP_LED_Off(LED_YELLOW);

        while(HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
        {
          BSP_LED_On(LED_GREEN);
        }
        BSP_LED_Off(LED_GREEN);// 綠燈不亮，表示 Raw Data 很快讀取完成
       
        // D-Cache invalidate
        SCB_InvalidateDCache_by_Addr(
            (uint32_t*)read_buffer,
            BUFFER_SIZE_BYTES
        );
		
		if(ValidateChunk() == 0)
        {
            break;
        }
		
		payload_offset = 0;

        for(i = 0; i < RECORD_COUNT; i++)
        {
            memcpy(&payload_buffer[payload_offset],
                   read_buffer[i].payload,
                   PAYLOAD_SIZE);

            payload_offset += PAYLOAD_SIZE;
        }		
          
        if(f_write(&MyFile,
                  payload_buffer,
                  payload_offset,
                  &bw) != FR_OK)
        {
            BSP_LED_On(LED_RED);
            while(1);
        }        

        // 下一段 Raw Sector
        current_sector += SD_BLOCK_COUNT;
    }

    //關閉檔案
    f_close(&MyFile);

    // PA6 LOW   檔案完成
    HAL_GPIO_WritePin(GPIOA,
                      GPIO_PIN_6,
                      GPIO_PIN_RESET);

    
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
/* USER CODE BEGIN Boot_Mode_Sequence_0 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  int32_t timeout;
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_0 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  /* Wait until CPU2 boots and enters in stop mode or timeout*/
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
  if ( timeout < 0 )
  {
  Error_Handler();
  }
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_YELLOW);
  BSP_LED_Init(LED_RED);
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
/* USER CODE BEGIN Boot_Mode_Sequence_2 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
/* When system initialization is finished, Cortex-M7 will release Cortex-M4 by means of
HSEM notification */
/*HW semaphore Clock enable*/
__HAL_RCC_HSEM_CLK_ENABLE();
/*Take HSEM */
HAL_HSEM_FastTake(HSEM_ID_0);
/*Release HSEM in order to notify the CPU2(CM4)*/
HAL_HSEM_Release(HSEM_ID_0,0);
/* wait until CPU2 wakes up from stop mode */
timeout = 0xFFFF;
while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
if ( timeout < 0 )
{
Error_Handler();
}
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  HAL_Delay(2000);

  MX_GPIO_Init();
  
  BSP_LED_On(LED_GREEN);
  HAL_Delay(500);
  BSP_LED_Off(LED_GREEN); // GPIO 初始化通過

  MX_SDMMC1_SD_Init();
  
  BSP_LED_On(LED_YELLOW);
  HAL_Delay(500);
  BSP_LED_Off(LED_YELLOW);// SDMMC1 初始化通過

  MX_FATFS_Init();
  
  BSP_LED_On(LED_RED);
  HAL_Delay(500);
  BSP_LED_Off(LED_RED);// FATFS 初始化通過

  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */


  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t file_id;

    if(f_mount(&SDFatFS,
               (TCHAR const*)SDPath,
               1) != FR_OK)
    {
        BSP_LED_On(LED_RED);
        while(1);
        Error_Handler();
    }
    /*
    // mount 成功綠燈亮 1 秒
    BSP_LED_On(LED_GREEN);
    HAL_Delay(1000);
    BSP_LED_Off(LED_GREEN);
    */

    for(file_id = 0; file_id < MAX_FILES_TO_GENERATE; file_id++)  // 想要生成幾個1.7MB檔案，檔案內容資料有 32768 筆資料，修改 MAX_FILES_TO_GENERATE 數值即可
    {
        GenerateFatFsFile(file_id);

        /*
        HAL_GPIO_WritePin(GPIOA,
                      GPIO_PIN_6,
                      GPIO_PIN_SET);

        HAL_Delay(50);

        HAL_GPIO_WritePin(GPIOA,
                          GPIO_PIN_6,
                          GPIO_PIN_RESET);

        HAL_Delay(50);
        // 每生成一個 TXT： PA6 pulse 一次。
        */
    }
   

    while(1)
    {
      /*
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6);

        HAL_Delay(30);
      */
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 36;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 3;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SDMMC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDMMC1_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC1_Init 0 */

  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */

  /* USER CODE END SDMMC1_Init 1 */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
  hsd1.Init.ClockDiv = 4;
  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SDMMC1_Init 2 */
  if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
  {
      Error_Handler();
  }
  /* USER CODE END SDMMC1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);

  /*Configure GPIO pin : SD_Detect_Pin */
  GPIO_InitStruct.Pin = SD_Detect_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SD_Detect_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PA6 */
  GPIO_InitStruct.Pin = GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PA8 PA11 PA12 */
  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_11|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG1_FS;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_HFNMI_PRIVDEF);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

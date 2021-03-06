//***********************************************************
//* pid.c
//***********************************************************

//***********************************************************
//* Includes
//***********************************************************

#include "compiledefs.h"
#include <avr/pgmspace.h> 
#include <avr/io.h>
#include <stdbool.h>
#include <util/delay.h>
#include <stdlib.h>
#include "io_cfg.h"
#include "gyros.h"
#include "main.h"
#include "init.h"
#include "acc.h"
#include "imu.h"
#include "rc.h"
#include "mixer.h"
#include "isr.h"

//************************************************************
// Defines
//************************************************************

#define PID_SCALE 6					// Empirical amount to reduce the PID values by to make them most useful
#define STANDARDLOOP 3571.0			// T1 counts of 700Hz cycle time (2500000/700)
#define SAMPLE_RATE 500				// HPF filter constants
#define HPF_FC	20
#define HPF_Q	1
#define HPF_O	(2 * M_PI * HPF_FC / SAMPLE_RATE)
#define HPF_C	(HPF_Q / HPF_O)
#define HPF_L	(1 / HPF_Q / HPF_O)

//************************************************************
// Notes
//************************************************************
//
// Servo output range is 2500 to 5000, centered on 3750.
// RC and PID values are added to this then rescaled at the the output stage to 1000 to 2000.
// As such, the maximum usable value that the PID section can output is +/-1250.
// So working backwards, prior to rescaling (/64) the max values are +/-80,000.
// Prior to this, PID_Gyro_I_actual has been divided by 32 so the values are now +/- 2,560,000
// However the I-term gain can be up to 127 which means the values are now limited to +/-20,157 for full scale authority.
// For reference, a constant gyro value of 50 would go full scale in about 1 second at max gain of 127 if incremented at 400Hz.
// This seems about right for heading hold usage.
//
// On the KK2.1 the gyros are configured to read +/-2000 deg/sec at full scale, or 16.4 deg/sec for each LSB value.  
// I divide that by 16 to give 0.976 deg/sec for each digit the gyros show. So "50" is about 48.8 degrees per second.
// 360 deg/sec would give a reading of 368 on the sensor calibration screen. Full stick is about 1000 or so. 
// So with no division of stick value by "Axis rate", full stick would equate to (1000/368 * 360) = 978 deg/sec. 
// With axis rate set to 2, the stick amount is quartered (250) or 244 deg/sec. A value of 3 would result in 122 deg/sec. 
//
// Stick rates: /64 (15.25), /32 (30.5), /16 (61*), /8 (122), /4 (244), /2 (488), /1 (976)
		
//************************************************************
// Prototypes
//************************************************************

void Sensor_PID(uint32_t period);
void Calculate_PID(void);

//************************************************************
// Code
//************************************************************

// PID globals for each [Profile] and [axis]
int16_t PID_Gyros[FLIGHT_MODES][NUMBEROFAXIS];
int16_t PID_ACCs[FLIGHT_MODES][NUMBEROFAXIS];
int32_t	IntegralGyro[FLIGHT_MODES][NUMBEROFAXIS];	// PID I-terms (gyro) for each axis
int32_t	GyroDTerm[NUMBEROFAXIS];					// Gyro D-terms for each axis
float	IntegralAccVertf[FLIGHT_MODES];				// Integrated Acc Z
float 	gyroSmooth[NUMBEROFAXIS];					// Filtered gyro data
int32_t PID_AvgGyro[NUMBEROFAXIS];					// Averaged gyro data over last x loops
float 	GyroAvgNoise;								// Gyro noise value	

float HPF_V = 0;
float HPF_T = 0;
float HPF_I = 0;
float fsample = 0;
	
// Run each loop to average gyro data and also accVert data
void Sensor_PID(uint32_t period)
{
	float tempf1 = 0;
	float tempf2 = 0;
	float factor = 0;						// Interval in seconds since the last loop
	float gyroADCf = 0;
	int8_t i = 0;
	int8_t	axis = 0;	
	int16_t	stick_P1 = 0;
	int16_t	stick_P2 = 0;
	int32_t P1_temp = 0;
	int32_t P2_temp = 0;
	
	// Cross-reference table for actual RCinput elements
	// Note that axes are reversed here with respect to their gyros
	// So why is AILERON different? Well on the KK hardware the sensors are arranged such that
	// RIGHT roll = +ve gyro, UP pitch = +ve gyro and LEFT yaw = +ve gyro.
	// However the way we have organised stick polarity, RIGHT roll and yaw are +ve, and DOWN elevator is too.
	// When combining with the gyro signals, the sticks have to be in the opposite polarity as the gyros.
	// As described above, pitch and yaw are already opposed, but roll needs to be reversed.

	int16_t	RCinputsAxis[NUMBEROFAXIS] = {-RCinputs[AILERON], RCinputs[ELEVATOR], RCinputs[RUDDER]};
	
	int8_t Stick_rates[FLIGHT_MODES][NUMBEROFAXIS] =
	{
		{Config.FlightMode[P1].Roll_Rate, Config.FlightMode[P1].Pitch_Rate, Config.FlightMode[P1].Yaw_Rate},
		{Config.FlightMode[P2].Roll_Rate, Config.FlightMode[P2].Pitch_Rate, Config.FlightMode[P2].Yaw_Rate}
	};

	//************************************************************
	// Create a measure of gyro noise
	//************************************************************

	// Only bother when display vibration info is set to "ON"
	if (Config.Vibration == ON)
	{
		// Work out quick average of all raw gyros and take the absolute value
		fsample = (float)(gyroADC_raw[ROLL] + gyroADC_raw[PITCH] + gyroADC_raw[YAW]);

		// HPF example from http://www.codeproject.com/Tips/681745/Csharp-Discrete-Time-RLC-Low-High-Pass-Filter-Rout
		// Some values preset for a 10Hz cutoff at 500Hz sample rate
		HPF_T = (fsample * HPF_O) - HPF_V;
		HPF_V += (HPF_I + HPF_T) / HPF_C;
		HPF_I += HPF_T / HPF_L;
		fsample -= HPF_V / HPF_O;

		// LPF filter the readings so that they are more persistent
		GyroAvgNoise = ((GyroAvgNoise * 99.0f) + abs(fsample)) / 100.0f;

		// Limit noise reading to 999
		if (GyroAvgNoise > 999.0f)
		{
			GyroAvgNoise = 999.0f;
		}
	}

	for (axis = 0; axis <= YAW; axis ++)
	{
		//************************************************************
		// Work out stick rate divider. 0 is slowest, 7 is fastest.
		// /64 (15.25), /32 (30.5), /16 (61*), /8 (122), /4 (244), /2 (488), /1 (976), *2 (1952)
		//************************************************************

		if (Stick_rates[P1][axis] <= 6)
		{
			stick_P1 = RCinputsAxis[axis] >> (4 - (Stick_rates[P1][axis] - 2));
		}
		else
		{
			stick_P1 = RCinputsAxis[axis] << ((Stick_rates[P1][axis]) - 6);
		}
		
		if (Stick_rates[P2][axis] <= 6)
		{
			stick_P2 = RCinputsAxis[axis] >> (4 - (Stick_rates[P2][axis] - 2));
		}
		else
		{
			stick_P2 = RCinputsAxis[axis] << ((Stick_rates[P2][axis]) - 6);
		}		
		
		//************************************************************
		// Gyro LPF
		//************************************************************	

		// Lookup LPF value
		// Note: Two sets of values for normal and high-speed mode
		if (Config.Servo_rate != FAST)
		{
			memcpy_P(&tempf1, &LPF_lookup[Config.Gyro_LPF], sizeof(float));
		}
		else
		{
			memcpy_P(&tempf1, &LPF_lookup_HS[Config.Gyro_LPF], sizeof(float));
		}		
			
		gyroADCf = gyroADC[axis]; // Promote gyro signal to suit

		if (Config.Gyro_LPF != NOFILTER)
		{
			// Gyro LPF
			gyroSmooth[axis] = ((gyroSmooth[axis] * (tempf1 - 1.0f)) + gyroADCf) / tempf1;
		}
		else
		{
			// Use raw gyroADC[axis] as source for gyro values when filter is off
			gyroSmooth[axis] = gyroADCf;
		}		
		
		// Demote back to int16_t
		gyroADC[axis] = (int16_t)gyroSmooth[axis];	

		//************************************************************
		// Magically correlate the I-term value with the loop rate.
		// This keeps the I-term and stick input constant over varying 
		// loop rates 
		//************************************************************

		P1_temp = gyroADC[axis] + stick_P1;
		P2_temp = gyroADC[axis] + stick_P2;
		
		// Work out multiplication factor compared to standard loop time
		tempf2 = period;							// Promote int32_t to float
		factor = period/STANDARDLOOP;
		
		// Adjust gyro and stick values based on factor		
		tempf2 = P1_temp;							// Promote int32_t to float
		tempf2 = tempf2 * factor;
		P1_temp = (int32_t)tempf2;					// Demote to int32_t
		
		tempf2 = P2_temp;
		tempf2 = tempf2 * factor;
		P2_temp = (int32_t)tempf2;

		//************************************************************
		// Increment gyro I-terms
		//************************************************************
		
		// Calculate I-term from gyro and stick data 
		// These may look similar, but they are constrained quite differently.
		IntegralGyro[P1][axis] += P1_temp;
		IntegralGyro[P2][axis] += P2_temp;

		//************************************************************
		// Limit the I-terms to the user-set limits
		//************************************************************
		
		for (i = P1; i <= P2; i++)
		{
			if (IntegralGyro[i][axis] > Config.Raw_I_Constrain[i][axis])
			{
				IntegralGyro[i][axis] = Config.Raw_I_Constrain[i][axis];
			}
			
			if (IntegralGyro[i][axis] < -Config.Raw_I_Constrain[i][axis])
			{
				IntegralGyro[i][axis] = -Config.Raw_I_Constrain[i][axis];
			}
		}

		//************************************************************
		// Sum gyro readings for P-terms for later averaging
		//************************************************************

		PID_AvgGyro[axis] += gyroADC[axis];
	
	} // for (axis = 0; axis <= YAW; axis ++)
		
	//************************************************************
	// Calculate the Z-acc I-term 
	// accVert is already smoothed by AccSmooth, but needs DC 
	// offsets removed to minimize drift.
	// Also, shrink the integral by a small fraction to temper 
	// remaining offsets.
	//************************************************************		
/*
	if (Config.AccVertFilter == ON)
	{
		IntegralAccVertf[P1] += (accVertf + accVertZerof);			// Remove current DC offset from accVert
		IntegralAccVertf[P2] += (accVertf + accVertZerof);		
	}

	else
	{
*/		IntegralAccVertf[P1] += accVertf;
		IntegralAccVertf[P2] += accVertf;		
//	}

/*		
	// Calculate the correct decimator number so that the current max I value
	// decimates in 10s. intervalf is the interval in seconds
	
	// Convert (period) from units of 400ns (1/2500000) to seconds (10s/400ns = 25000000)
	tempf1 = period;					// Promote uint32_t to float
	intervalf = tempf1/25000000.0f;		// This gives the period in 1/10 seconds
		
	tempf1 = Config.Raw_I_Constrain[P1][ZED];
	tempf1 = tempf1 * intervalf;

	tempf2 = Config.Raw_I_Constrain[P2][ZED];
	tempf2 = tempf2 * intervalf;

	if (IntegralAccVertf[P1] > 0)
	{
		IntegralAccVertf[P1] = IntegralAccVertf[P1] - (int32_t)tempf1;		// Decimator. Shrink integrals within 10s
	}
	else
	{
		IntegralAccVertf[P1] = IntegralAccVertf[P1] + (int32_t)tempf1;
	}
	
	if (IntegralAccVertf[P2] > 0)
	{
		IntegralAccVertf[P2] = IntegralAccVertf[P2] - (int32_t)tempf2;	
	}
	else
	{
		IntegralAccVertf[P2] = IntegralAccVertf[P2] + (int32_t)tempf2;
	}
*/	

/*	
	IntegralAccVertf[P1] = IntegralAccVertf[P1] * 0.9995f;			// Decimator. Shrink integrals by .05%
	IntegralAccVertf[P2] = IntegralAccVertf[P2] * 0.9995f;
*/
	tempf1 = Config.AccVertFilter;	// Promote AccVertfilter (0 to 127)
	tempf1 = tempf1 / 10000.0f;
	tempf1 = 1.0f - tempf1;
	
	IntegralAccVertf[P1] = IntegralAccVertf[P1] * tempf1;			// Decimator. Shrink integrals by user-set amount
	IntegralAccVertf[P2] = IntegralAccVertf[P2] * tempf1;

	
	//************************************************************
	// Limit the Z-acc I-terms to the user-set limits
	//************************************************************
	for (i = P1; i <= P2; i++)
	{
		tempf1 = Config.Raw_I_Constrain[i][ZED];	// Promote
		
		if (IntegralAccVertf[i] > tempf1)
		{
			IntegralAccVertf[i] = tempf1;
		}
			
		if (IntegralAccVertf[i] < -tempf1)
		{
			IntegralAccVertf[i] = -tempf1;
		}
	}
}

// Run just before PWM output, using averaged data
void Calculate_PID(void)
{
	int32_t PID_gyro_temp1 = 0;				// P1
	int32_t PID_gyro_temp2 = 0;				// P2
	int32_t PID_acc_temp1 = 0;				// P
	int32_t PID_acc_temp2 = 0;				// I
	int32_t PID_Gyro_I_actual1 = 0;			// Actual unbound i-terms P1
	int32_t PID_Gyro_I_actual2 = 0;			// P2
	int8_t	axis = 0;
	int8_t i = 0;

	// Initialise arrays with gain values.
	int8_t 	P_gain[FLIGHT_MODES][NUMBEROFAXIS] = 
		{
			{Config.FlightMode[P1].Roll_P_mult, Config.FlightMode[P1].Pitch_P_mult, Config.FlightMode[P1].Yaw_P_mult},
		 	{Config.FlightMode[P2].Roll_P_mult, Config.FlightMode[P2].Pitch_P_mult, Config.FlightMode[P2].Yaw_P_mult}
		};

	int8_t 	I_gain[FLIGHT_MODES][NUMBEROFAXIS+1] = 
		{
			{Config.FlightMode[P1].Roll_I_mult, Config.FlightMode[P1].Pitch_I_mult, Config.FlightMode[P1].Yaw_I_mult, Config.FlightMode[P1].A_Zed_I_mult},
			{Config.FlightMode[P2].Roll_I_mult, Config.FlightMode[P2].Pitch_I_mult, Config.FlightMode[P2].Yaw_I_mult, Config.FlightMode[P2].A_Zed_I_mult}
		};

	int8_t 	L_gain[FLIGHT_MODES][NUMBEROFAXIS] = 
		{
			{Config.FlightMode[P1].A_Roll_P_mult, Config.FlightMode[P1].A_Pitch_P_mult, Config.FlightMode[P1].A_Zed_P_mult},
			{Config.FlightMode[P2].A_Roll_P_mult, Config.FlightMode[P2].A_Pitch_P_mult, Config.FlightMode[P2].A_Zed_P_mult}
		};

	// Only for roll and pitch acc trim
	int16_t	L_trim[FLIGHT_MODES][2] =
		{
			{Config.Rolltrim[P1], Config.Pitchtrim[P1]},
			{Config.Rolltrim[P2], Config.Pitchtrim[P2]}
		};

	//************************************************************
	// PID loop
	//************************************************************
	
	for (axis = 0; axis <= YAW; axis ++)
	{
		//************************************************************
		// Get average gyro readings for P-terms
		//************************************************************

		gyroADC[axis] = (int16_t)(PID_AvgGyro[axis] / LoopCount);
		PID_AvgGyro[axis] = 0;					// Reset average

		//************************************************************
		// Add in gyro Yaw trim
		//************************************************************

		if (axis == YAW)
		{
			PID_gyro_temp1 = (int32_t)(Config.FlightMode[P1].Yaw_trim << 6);
			PID_gyro_temp2 = (int32_t)(Config.FlightMode[P2].Yaw_trim << 6);
		}
		// Reset PID_gyro variables to that data does not accumulate cross-axis
		else
		{
			PID_gyro_temp1 = 0;
			PID_gyro_temp2 = 0;
		}

		//************************************************************
		// Calculate PID gains
		//************************************************************

		// Gyro P-term													// Profile P1
		PID_gyro_temp1 += gyroADC[axis] * P_gain[P1][axis];				// Multiply P-term (Max gain of 127)
		PID_gyro_temp1 = PID_gyro_temp1 * (int32_t)3;					// Multiply by 3

		// Gyro I-term
		PID_Gyro_I_actual1 = IntegralGyro[P1][axis] * I_gain[P1][axis];	// Multiply I-term (Max gain of 127)
		PID_Gyro_I_actual1 = PID_Gyro_I_actual1 >> 5;					// Divide by 32

		// Gyro P-term
		PID_gyro_temp2 += gyroADC[axis] * P_gain[P2][axis];				// Profile P2
		PID_gyro_temp2 = PID_gyro_temp2 * (int32_t)3;

		// Gyro I-term
		PID_Gyro_I_actual2 = IntegralGyro[P2][axis] * I_gain[P2][axis];
		PID_Gyro_I_actual2 = PID_Gyro_I_actual2 >> 5;

		//************************************************************
		// I-term output limits
		//************************************************************

		// P1 limits
		if (PID_Gyro_I_actual1 > Config.Raw_I_Limits[P1][axis]) 
		{
			PID_Gyro_I_actual1 = Config.Raw_I_Limits[P1][axis];
		}
		else if (PID_Gyro_I_actual1 < -Config.Raw_I_Limits[P1][axis]) 
		{
			PID_Gyro_I_actual1 = -Config.Raw_I_Limits[P1][axis];	
		}

		// P2 limits
		if (PID_Gyro_I_actual2 > Config.Raw_I_Limits[P2][axis]) 
		{
			PID_Gyro_I_actual2 = Config.Raw_I_Limits[P2][axis];
		}
		else if (PID_Gyro_I_actual2 < -Config.Raw_I_Limits[P2][axis]) 
		{
			PID_Gyro_I_actual2 = -Config.Raw_I_Limits[P2][axis];	
		}

		//************************************************************
		// Sum Gyro P, I and D terms and rescale
		//************************************************************

		PID_Gyros[P1][axis] = (int16_t)((PID_gyro_temp1 + PID_Gyro_I_actual1) >> PID_SCALE); // Currently PID_SCALE = 6 so /64
		PID_Gyros[P2][axis] = (int16_t)((PID_gyro_temp2 + PID_Gyro_I_actual2) >> PID_SCALE);

		//************************************************************
		// Calculate error from angle data and trim (roll and pitch only)
		//************************************************************

		if (axis < YAW)
		{
			// Do for P1 and P2
			for (i = P1; i <= P2; i++)
			{
				PID_acc_temp1 = angle[axis] - L_trim[i][axis];				// Offset angle with trim
				PID_acc_temp1 *= L_gain[i][axis];							// P-term of accelerometer (Max gain of 127)
				PID_ACCs[i][axis] = (int16_t)(PID_acc_temp1 >> 8);			// Reduce and convert to integer
			}
		}

	} // PID loop (axis)

	//************************************************************
	// Calculate an Acc-Z PI value 
	//************************************************************

	// Do for P1 and P2
	for (i = P1; i <= P2; i++)
	{
		// P-term
		PID_acc_temp1 = (int32_t)-accVertf;					// Zeroed AccSmooth signal. Negate to oppose G
		PID_acc_temp1 *= L_gain[i][YAW];					// Multiply P-term (Max gain of 127)
		PID_acc_temp1 = PID_acc_temp1 * (int32_t)3;			// Multiply by 3

		// I-term
		PID_acc_temp2 = (int32_t)-IntegralAccVertf[i];		// Get and copy integrated Z-acc value. Negate to oppose G
		PID_acc_temp2 *= I_gain[i][ZED];					// Multiply I-term (Max gain of 127)
		PID_acc_temp2 = PID_acc_temp2 >> 2;					// Divide by 4

		if (PID_acc_temp2 > Config.Raw_I_Limits[i][ZED])	// Limit I-term outputs to user-set percentage
		{
			PID_acc_temp2 = Config.Raw_I_Limits[i][ZED];
		}
		if (PID_acc_temp2 < -Config.Raw_I_Limits[i][ZED])
		{
			PID_acc_temp2 = -Config.Raw_I_Limits[i][ZED];
		}

		// Formulate PI value and scale
		PID_ACCs[i][YAW] = (int16_t)((PID_acc_temp1 + PID_acc_temp2) >> PID_SCALE); // Copy to global values
	}
}

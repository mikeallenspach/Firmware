/****************************************************************************
 *
 *   Copyright (c) 2012-2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mixer_multirotor.cpp
 *
 * Multi-rotor mixers.
 */

#include "MultirotorMixer.hpp"

#include <float.h>
#include <cstring>
#include <cstdio>

#include <mathlib/mathlib.h>

#ifdef MIXER_MULTIROTOR_USE_MOCK_GEOMETRY
enum class MultirotorGeometry : MultirotorGeometryUnderlyingType {
	QUAD_X,
	MAX_GEOMETRY
};
namespace
{
const MultirotorMixer::Rotor _config_quad_x[] = {
	{ -0.707107,  0.707107,  1.000000,  1.000000 },
	{  0.707107, -0.707107,  1.000000,  1.000000 },
	{  0.707107,  0.707107, -1.000000,  1.000000 },
	{ -0.707107, -0.707107, -1.000000,  1.000000 },
};
const MultirotorMixer::Rotor *_config_index[] = {
	&_config_quad_x[0]
};
const unsigned _config_rotor_count[] = {4};
const char *_config_key[] = {"4x"};
}

#else

// This file is generated by the px_generate_mixers.py script which is invoked during the build process
// #include "mixer_multirotor.generated.h"
#include "mixer_multirotor_normalized.generated.h"

#endif /* MIXER_MULTIROTOR_USE_MOCK_GEOMETRY */


#define debug(fmt, args...)	do { } while(0)
//#define debug(fmt, args...)	do { printf("[mixer] " fmt "\n", ##args); } while(0)
//#include <debug.h>
//#define debug(fmt, args...)	syslog(fmt "\n", ##args)

MultirotorMixer::MultirotorMixer(ControlCallback control_cb, uintptr_t cb_handle, MultirotorGeometry geometry,
				 float roll_scale, float pitch_scale, float yaw_scale, float idle_speed) :
	MultirotorMixer(control_cb, cb_handle, _config_index[(int)geometry], _config_rotor_count[(int)geometry])
{
	_roll_scale = roll_scale;
	_pitch_scale = pitch_scale;
	_yaw_scale = yaw_scale;
	_idle_speed = -1.0f + idle_speed * 2.0f;	/* shift to output range here to avoid runtime calculation */
}

MultirotorMixer::MultirotorMixer(ControlCallback control_cb, uintptr_t cb_handle, const Rotor *rotors,
				 unsigned rotor_count) :
	Mixer(control_cb, cb_handle),
	_rotor_count(rotor_count),
	_rotors(rotors),
	_outputs_prev(new float[_rotor_count]),
	_tmp_array(new float[_rotor_count])
{
	for (unsigned i = 0; i < _rotor_count; ++i) {
		_outputs_prev[i] = _idle_speed;
	}
}

MultirotorMixer::~MultirotorMixer()
{
	delete[] _outputs_prev;
	delete[] _tmp_array;
}

MultirotorMixer *
MultirotorMixer::from_text(Mixer::ControlCallback control_cb, uintptr_t cb_handle, const char *buf, unsigned &buflen)
{
	MultirotorGeometry geometry = MultirotorGeometry::MAX_GEOMETRY;
	char geomname[8];
	int s[4];
	int used;

	/* enforce that the mixer ends with a new line */
	if (!string_well_formed(buf, buflen)) {
		return nullptr;
	}

	if (sscanf(buf, "R: %7s %d %d %d %d%n", geomname, &s[0], &s[1], &s[2], &s[3], &used) != 5) {
		debug("multirotor parse failed on '%s'", buf);
		return nullptr;
	}

	if (used > (int)buflen) {
		debug("OVERFLOW: multirotor spec used %d of %u", used, buflen);
		return nullptr;
	}

	buf = skipline(buf, buflen);

	if (buf == nullptr) {
		debug("no line ending, line is incomplete");
		return nullptr;
	}

	debug("remaining in buf: %d, first char: %c", buflen, buf[0]);

	for (MultirotorGeometryUnderlyingType i = 0; i < (MultirotorGeometryUnderlyingType)MultirotorGeometry::MAX_GEOMETRY;
	     i++) {
		if (!strcmp(geomname, _config_key[i])) {
			geometry = (MultirotorGeometry)i;
			break;
		}
	}

	if (geometry == MultirotorGeometry::MAX_GEOMETRY) {
		debug("unrecognised geometry '%s'", geomname);
		return nullptr;
	}

	debug("adding multirotor mixer '%s'", geomname);

	return new MultirotorMixer(
		       control_cb,
		       cb_handle,
		       geometry,
		       s[0] / 10000.0f,
		       s[1] / 10000.0f,
		       s[2] / 10000.0f,
		       s[3] / 10000.0f);
}

float
MultirotorMixer::compute_desaturation_gain(const float *desaturation_vector, const float *outputs,
		saturation_status &sat_status, float min_output, float max_output) const
{
	float k_min = 0.f;
	float k_max = 0.f;

	for (unsigned i = 0; i < _rotor_count; i++) {
		// Avoid division by zero. If desaturation_vector[i] is zero, there's nothing we can do to unsaturate anyway
		if (fabsf(desaturation_vector[i]) < FLT_EPSILON) {
			continue;
		}

		if (outputs[i] < min_output) {
			float k = (min_output - outputs[i]) / desaturation_vector[i];

			if (k < k_min) { k_min = k; }

			if (k > k_max) { k_max = k; }

			sat_status.flags.motor_neg = true;
		}

		if (outputs[i] > max_output) {
			float k = (max_output - outputs[i]) / desaturation_vector[i];

			if (k < k_min) { k_min = k; }

			if (k > k_max) { k_max = k; }

			sat_status.flags.motor_pos = true;
		}
	}

	// Reduce the saturation as much as possible
	return k_min + k_max;
}

void
MultirotorMixer::minimize_saturation(const float *desaturation_vector, float *outputs,
				     saturation_status &sat_status, float min_output, float max_output, bool reduce_only) const
{
	float k1 = compute_desaturation_gain(desaturation_vector, outputs, sat_status, min_output, max_output);

	if (reduce_only && k1 > 0.f) {
		return;
	}

	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] += k1 * desaturation_vector[i];
	}

	// Compute the desaturation gain again based on the updated outputs.
	// In most cases it will be zero. It won't be if max(outputs) - min(outputs) > max_output - min_output.
	// In that case adding 0.5 of the gain will equilibrate saturations.
	float k2 = 0.5f * compute_desaturation_gain(desaturation_vector, outputs, sat_status, min_output, max_output);

	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] += k2 * desaturation_vector[i];
	}
}

void
MultirotorMixer::mix_airmode_rp(float roll, float pitch, float yaw, float thrust, float *outputs)
{
	// Airmode for roll and pitch, but not yaw

	// Mix without yaw
	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] = roll * _rotors[i].roll_scale +
			     pitch * _rotors[i].pitch_scale +
			     thrust * _rotors[i].thrust_scale;

		// Thrust will be used to unsaturate if needed
		_tmp_array[i] = _rotors[i].thrust_scale;
	}

	minimize_saturation(_tmp_array, outputs, _saturation_status);

	// Mix yaw independently
	mix_yaw(yaw, outputs);
}

void
MultirotorMixer::mix_airmode_rpy(float roll, float pitch, float yaw, float thrust, float *outputs)
{
	// Airmode for roll, pitch and yaw

	// Do full mixing
	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] = roll * _rotors[i].roll_scale +
			     pitch * _rotors[i].pitch_scale +
			     yaw * _rotors[i].yaw_scale +
			     thrust * _rotors[i].thrust_scale;

		// Thrust will be used to unsaturate if needed
		_tmp_array[i] = _rotors[i].thrust_scale;
	}

	minimize_saturation(_tmp_array, outputs, _saturation_status);

	// Unsaturate yaw (in case upper and lower bounds are exceeded)
	// to prioritize roll/pitch over yaw.
	for (unsigned i = 0; i < _rotor_count; i++) {
		_tmp_array[i] = _rotors[i].yaw_scale;
	}

	minimize_saturation(_tmp_array, outputs, _saturation_status);
}

void
MultirotorMixer::mix_airmode_disabled(float roll, float pitch, float yaw, float thrust, float *outputs)
{
	// Airmode disabled: never allow to increase the thrust to unsaturate a motor

	// Mix without yaw
	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] = roll * _rotors[i].roll_scale +
			     pitch * _rotors[i].pitch_scale +
			     thrust * _rotors[i].thrust_scale;

		// Thrust will be used to unsaturate if needed
		_tmp_array[i] = _rotors[i].thrust_scale;
	}

	// only reduce thrust
	minimize_saturation(_tmp_array, outputs, _saturation_status, 0.f, 1.f, true);

	// Reduce roll/pitch acceleration if needed to unsaturate
	for (unsigned i = 0; i < _rotor_count; i++) {
		_tmp_array[i] = _rotors[i].roll_scale;
	}

	minimize_saturation(_tmp_array, outputs, _saturation_status);

	for (unsigned i = 0; i < _rotor_count; i++) {
		_tmp_array[i] = _rotors[i].pitch_scale;
	}

	minimize_saturation(_tmp_array, outputs, _saturation_status);

	// Mix yaw independently
	mix_yaw(yaw, outputs);
}

void MultirotorMixer::mix_yaw(float yaw, float *outputs)
{
	// Add yaw to outputs
	for (unsigned i = 0; i < _rotor_count; i++) {
		outputs[i] += yaw * _rotors[i].yaw_scale;

		// Yaw will be used to unsaturate if needed
		_tmp_array[i] = _rotors[i].yaw_scale;
	}

	// Change yaw acceleration to unsaturate the outputs if needed (do not change roll/pitch),
	// and allow some yaw response at maximum thrust
	minimize_saturation(_tmp_array, outputs, _saturation_status, 0.f, 1.15f);

	for (unsigned i = 0; i < _rotor_count; i++) {
		_tmp_array[i] = _rotors[i].thrust_scale;
	}

	// reduce thrust only
	minimize_saturation(_tmp_array, outputs, _saturation_status, 0.f, 1.f, true);
}

void MultirotorMixer::mix_vtol(float *outputs){

    // platform parameters
    float h_0   = 0.015f;
    float L_0   = 0.29f;
    float l_1   = 0.1575f;
    float l_3   = 0.105f;
    float l_4   = 0.105f;
    float C_T   = 1.11919e-5f;
    float C_Q   = 1.99017e-7f;
    float C     = C_Q / C_T;
    float d_chi_max = math::radians(10.0f);

    //aerodynamics
    float C_La  = 0.058649f;
    float C_Me  = 0.55604f;
    float C_Nr  = 0.055604f;
    float S     = 0.4266f;
    float b     = 2.0f;
    float c_bar = 0.2f;

    float L_factor = C_La * S * b;
    float M_factor = C_Me * S * c_bar;
    float N_factor = C_Nr * S * b;

    float delta_min = math::radians(-35.0f);
    float delta_max = math::radians(35.0f);
    
    // load inputs
	float L         = math::constrain(get_control(0, 0), -1.0f, 1.0f);
	float M         = math::constrain(get_control(0, 1), -1.0f, 1.0f);
	float N         = math::constrain(get_control(0, 2), -1.0f, 1.0f);

    float T         = math::constrain(get_control(0, 3), 0.0f, 1.0f);
    float chi_cmd   = math::constrain(get_control(0,4), -1.0f, 1.0f);

	float airspd    = math::constrain(get_control(0, 5), 1e-8f, 1.0f);

    // denormalize
    float T_MAX     = 48.0f;
    float chi_MAX   = math::radians(90.0f);
    float M_MAX     = 2.0f;
    float AIRSPD_MAX= 40.0f;

    L       *= M_MAX;
    M       *= M_MAX;
    N       *= M_MAX;

    T       *= T_MAX;
    chi_cmd *= chi_MAX;

    airspd  *= AIRSPD_MAX;

    // control surface deflections
    float q_bar = 0.5f * 1.2f * airspd * airspd;
    float L_ = L_factor * q_bar;
    float M_ = M_factor * q_bar;
    float N_ = N_factor * q_bar;

    // scale with airspeed to avoid bang-bang behaviour at low speeds
    float scale = math::constrain( (airspd - 4.0f)/6.0f, 0.0f, 1.0f);
    
    float delta_a = math::constrain( L/L_*scale,  delta_min, delta_max );
    float delta_e = math::constrain( M/M_*scale, delta_min, delta_max );
    float delta_r = math::constrain( N/N_*scale,   delta_min, delta_max );

    L -= L_ * delta_a;
    M -= M_ * delta_e;
    N -= N_ * delta_r;

    float t1, t2, t3, t4, d_chi_r, d_chi_l, chi_r, chi_l;

    float c_chi = cosf(chi_cmd);
    float s_chi = sinf(chi_cmd);
    float c_2chi = cosf(2.0f*chi_cmd);
    float s_2chi = sinf(2.0f*chi_cmd);
    float c_chi2 = c_chi * c_chi;
    float l_34   = l_3 + l_4;
    float l_1_2  = l_1 * l_1;
    float l_3_2  = l_3 * l_3;
    float l_4_2  = l_4 * l_4;

    float temp1  = 2.0f * l_1_2 + l_3 * l_4;
    float temp2  = 2.0f * temp1 + l_3_2 + l_4_2;
    
    float l_arm;
    float l_arm_2;
    

    // compute pseudoinverse A_pinv = A^T * inv( A * A^T )
    float A_pinv[40];

    float sign_1;
    float sign_2;
    float sign_3;
    float denom_1 = ( temp2 + 4.0f * c_chi * l_1 * l_34 );
    float denom_2 = 4.0f * ( C * C + L_0 * L_0 );

    for( int i = 0; i <= 7; i++ ){
        sign_1 = (i >= 2 && i <= 5) * 2.0f - 1.0f;
        sign_2 = (i >= 4) * 2.0f - 1.0f;
        sign_3 = ( i%4 == 0 || (i-1)%4 == 0 ) * 2.0f - 1.0f;
        l_arm  =  0.5f * ( 1 + sign_1 ) * l_3 + 0.5f * (1 - sign_1) * l_4;
        l_arm_2= l_arm * l_arm;

        if(i % 2 == 0){ // even rows
            // fist column
            A_pinv[0 + i*5] =
                (
                 temp2 * c_chi
                 - sign_1 * 2.0f * h_0 * l_34 * s_chi
                 + 4.0f * l_1 * l_34 * c_chi2
                )/ ( 4.0f * denom_1 );

            // second column
            A_pinv[1 + i*5] =
            -(  
                ( temp1 + l_arm_2 ) * s_chi
                + l_1 * l_34 * s_2chi
            )/ ( 2.0f * denom_1 );

            // third column
            A_pinv[2 + i*5] = (-sign_2 * L_0 * s_chi + sign_3 * C * c_chi) / denom_2;

            // fourth column
            A_pinv[3 + i*5] = -sign_1 * s_chi * l_34 / (2.0f * denom_1 );

            // fifth column
            A_pinv[4 + i*5] = (sign_2 * L_0 * c_chi + sign_3 * C * s_chi ) / denom_2;

        
        } else { // odd rows
            // first column
            A_pinv[0 + i*5] = 
                ( 
                 temp2 * s_chi
                 + sign_1 * 2.0f * h_0 * ( c_chi * l_34 + 2.0f * l_1)
                 + 2.0f * l_1 * l_34 * s_2chi
                )/( 4.0f * denom_1 );

            // second column
            A_pinv[1 + i*5] = 
            ( 
             2.0f * l_1 * l_arm 
             + (temp1 + l_arm_2) * c_chi
             + l_1 * l_34 * c_2chi
            )/( 2.0f * denom_1 );

            // third column
            A_pinv[2 + i*5] = (sign_2 * L_0 * c_chi + sign_3 * C * s_chi) / denom_2;

            // fourth column
            A_pinv[3 + i*5] = sign_1 * (2.0f * l_1 + l_34 * c_chi) / (2.0f * denom_1);

            // fifth column
            A_pinv[4 + i*5] = (sign_2 * L_0 * s_chi - sign_3 * C * c_chi ) / denom_2;
        }
    }

    //printf("A_pinv:\n");
    //for(int i = 0; i<=7; i++){
    //    printf("%f %f %f %f %f\n", (double)A_pinv[5*i], (double)A_pinv[5*i+1], 
    //            (double)A_pinv[5*i+2], (double)A_pinv[5*i+3], (double)A_pinv[5*i+4]);
    //}
    //printf("\n");
    //
    //printf("alloc T: %f\n",(double)T);
    //printf("alloc chi: %f\n",(double)chi_cmd);
    //printf("alloc L: %f\n",(double)L);
    //printf("alloc M: %f\n",(double)M);
    //printf("alloc N: %f\n",(double)N);
    
    
    float v[8];
    for( int i=0; i<=7; i++){
        v[i] =    A_pinv[5*i]     * T * sinf( chi_cmd )
                + A_pinv[5*i + 1] * T * cosf( chi_cmd )
                + A_pinv[5*i + 2] * L
                + A_pinv[5*i + 3] * M
                + A_pinv[5*i + 4] * N;
    }

    scale = math::constrain( 0.25f*(T - 2.0f), 0.0f, 1.0f);
    d_chi_r = scale * atan2f( v[0] + v[2] , v[1] + v[3] );
    d_chi_l = scale * atan2f( v[4] + v[6] , v[5] + v[7] );


    //printf("d_chi_r alloc: %f\n",(double)d_chi_r);
    //printf("d_chi_l alloc: %f\n",(double)d_chi_l);

    t1 = v[0] * sinf( d_chi_r ) + v[1] * cosf( d_chi_r );
    t2 = v[2] * sinf( d_chi_r ) + v[3] * cosf( d_chi_r );
    t3 = v[4] * sinf( d_chi_l ) + v[5] * cosf( d_chi_l );
    t4 = v[6] * sinf( d_chi_l ) + v[7] * cosf( d_chi_l );

	d_chi_r = math::constrain( d_chi_r, -d_chi_max, d_chi_max);
	d_chi_l = math::constrain( d_chi_l, -d_chi_max, d_chi_max);

    chi_r   = chi_cmd + d_chi_r;
    chi_l   = chi_cmd + d_chi_l;

    // scale thrust to PWM
    outputs[0] = -1.146746f+sqrtf(0.0821782f+0.355259f*t1);
    outputs[1] = -1.146746f+sqrtf(0.0821782f+0.355259f*t2);
    outputs[2] = -1.146746f+sqrtf(0.0821782f+0.355259f*t3);
    outputs[3] = -1.146746f+sqrtf(0.0821782f+0.355259f*t4);
    outputs[4] = -0.9602f * chi_l + 0.7106f;
    outputs[5] = 0.9602f  * chi_r - 0.7106f;
    outputs[6] = -(2.0f * delta_a - (delta_max + delta_min))/(delta_max - delta_min);
}

unsigned
MultirotorMixer::mix(float *outputs, unsigned space)
{
	if (space < _rotor_count) {
		return 0;
	}

	//float roll    = math::constrain(get_control(0, 0) * _roll_scale, -1.0f, 1.0f);
	//float pitch   = math::constrain(get_control(0, 1) * _pitch_scale, -1.0f, 1.0f);
	//float yaw     = math::constrain(get_control(0, 2) * _yaw_scale, -1.0f, 1.0f);
	//float thrust  = math::constrain(get_control(0, 3), 0.0f, 1.0f);

	// clean out class variable used to capture saturation
	_saturation_status.value = 0;

    mix_vtol(outputs);

	// check for tilt angle saturation against slew rate limits
	if (_delta_out_max > 0.0f) {
        // left side tilt saturation
		float delta_out_chi_l = outputs[4] - _outputs_prev[4];
		if (delta_out_chi_l > _delta_out_max) {
			outputs[4] = _outputs_prev[4] + _delta_out_max;

		} else if (delta_out_chi_l < -_delta_out_max) {
			outputs[4] = _outputs_prev[4] - _delta_out_max;
		}

        // right side tilt saturation
		float delta_out_chi_r = outputs[5] - _outputs_prev[5];
		if (delta_out_chi_r > _delta_out_max) {
			outputs[5] = _outputs_prev[5] + _delta_out_max;
		} else if (delta_out_chi_r < -_delta_out_max) {
			outputs[5] = _outputs_prev[5] - _delta_out_max;
		}
	}

	_outputs_prev[4] = outputs[4];
	_outputs_prev[5] = outputs[5];


	// this will force the caller of the mixer to always supply new slew rate values, otherwise no slew rate limiting will happen
	_delta_out_max = 0.0f;

    return 7;

	// Do the mixing using the strategy given by the current Airmode configuration
	//switch (_airmode) {
	//case Airmode::roll_pitch:
	//	mix_airmode_rp(roll, pitch, yaw, thrust, outputs);
	//	break;

	//case Airmode::roll_pitch_yaw:
	//	mix_airmode_rpy(roll, pitch, yaw, thrust, outputs);
	//	break;

	//case Airmode::disabled:
	//default: // just in case: default to disabled
	//	mix_airmode_disabled(roll, pitch, yaw, thrust, outputs);
	//	break;
	//}

	//Apply thrust model and scale outputs to range [idle_speed, 1].
	//At this point the outputs are expected to be in [0, 1], but they can be outside, for example
	//if a roll command exceeds the motor band limit.
	//for (unsigned i = 0; i < _rotor_count; i++) {
	//	// Implement simple model for static relationship between applied motor pwm and motor thrust
	//	// model: thrust = (1 - _thrust_factor) * PWM + _thrust_factor * PWM^2
	//	if (_thrust_factor > 0.0f) {
	//		outputs[i] = -(1.0f - _thrust_factor) / (2.0f * _thrust_factor) + sqrtf((1.0f - _thrust_factor) *
	//				(1.0f - _thrust_factor) / (4.0f * _thrust_factor * _thrust_factor) + (outputs[i] < 0.0f ? 0.0f : outputs[i] /
	//						_thrust_factor));
	//	}

	//	outputs[i] = math::constrain(_idle_speed + (outputs[i] * (1.0f - _idle_speed)), _idle_speed, 1.0f);
	//}

	//// Slew rate limiting and saturation checking
	//for (unsigned i = 0; i < _rotor_count; i++) {
	//	bool clipping_high = false;
	//	bool clipping_low_roll_pitch = false;
	//	bool clipping_low_yaw = false;

	//	// Check for saturation against static limits.
	//	// We only check for low clipping if airmode is disabled (or yaw
	//	// clipping if airmode==roll/pitch), since in all other cases thrust will
	//	// be reduced or boosted and we can keep the integrators enabled, which
	//	// leads to better tracking performance.
	//	if (outputs[i] < _idle_speed + 0.01f) {
	//		if (_airmode == Airmode::disabled) {
	//			clipping_low_roll_pitch = true;
	//			clipping_low_yaw = true;

	//		} else if (_airmode == Airmode::roll_pitch) {
	//			clipping_low_yaw = true;
	//		}
	//	}

	//	// check for saturation against slew rate limits
	//	if (_delta_out_max > 0.0f) {
	//		float delta_out = outputs[i] - _outputs_prev[i];

	//		if (delta_out > _delta_out_max) {
	//			outputs[i] = _outputs_prev[i] + _delta_out_max;
	//			clipping_high = true;

	//		} else if (delta_out < -_delta_out_max) {
	//			outputs[i] = _outputs_prev[i] - _delta_out_max;
	//			clipping_low_roll_pitch = true;
	//			clipping_low_yaw = true;
	//		}
	//	}

	//	_outputs_prev[i] = outputs[i];

	//	// update the saturation status report
	//	update_saturation_status(i, clipping_high, clipping_low_roll_pitch, clipping_low_yaw);
	//}

	// this will force the caller of the mixer to always supply new slew rate values, otherwise no slew rate limiting will happen
	//_delta_out_max = 0.0f;

	//return _rotor_count;
}

/*
 * This function update the control saturation status report using the following inputs:
 *
 * index: 0 based index identifying the motor that is saturating
 * clipping_high: true if the motor demand is being limited in the positive direction
 * clipping_low_roll_pitch: true if the motor demand is being limited in the negative direction (roll/pitch)
 * clipping_low_yaw: true if the motor demand is being limited in the negative direction (yaw)
*/
void
MultirotorMixer::update_saturation_status(unsigned index, bool clipping_high, bool clipping_low_roll_pitch,
		bool clipping_low_yaw)
{
	// The motor is saturated at the upper limit
	// check which control axes and which directions are contributing
	if (clipping_high) {
		if (_rotors[index].roll_scale > 0.0f) {
			// A positive change in roll will increase saturation
			_saturation_status.flags.roll_pos = true;

		} else if (_rotors[index].roll_scale < 0.0f) {
			// A negative change in roll will increase saturation
			_saturation_status.flags.roll_neg = true;
		}

		// check if the pitch input is saturating
		if (_rotors[index].pitch_scale > 0.0f) {
			// A positive change in pitch will increase saturation
			_saturation_status.flags.pitch_pos = true;

		} else if (_rotors[index].pitch_scale < 0.0f) {
			// A negative change in pitch will increase saturation
			_saturation_status.flags.pitch_neg = true;
		}

		// check if the yaw input is saturating
		if (_rotors[index].yaw_scale > 0.0f) {
			// A positive change in yaw will increase saturation
			_saturation_status.flags.yaw_pos = true;

		} else if (_rotors[index].yaw_scale < 0.0f) {
			// A negative change in yaw will increase saturation
			_saturation_status.flags.yaw_neg = true;
		}

		// A positive change in thrust will increase saturation
		_saturation_status.flags.thrust_pos = true;
	}

	// The motor is saturated at the lower limit
	// check which control axes and which directions are contributing
	if (clipping_low_roll_pitch) {
		// check if the roll input is saturating
		if (_rotors[index].roll_scale > 0.0f) {
			// A negative change in roll will increase saturation
			_saturation_status.flags.roll_neg = true;

		} else if (_rotors[index].roll_scale < 0.0f) {
			// A positive change in roll will increase saturation
			_saturation_status.flags.roll_pos = true;
		}

		// check if the pitch input is saturating
		if (_rotors[index].pitch_scale > 0.0f) {
			// A negative change in pitch will increase saturation
			_saturation_status.flags.pitch_neg = true;

		} else if (_rotors[index].pitch_scale < 0.0f) {
			// A positive change in pitch will increase saturation
			_saturation_status.flags.pitch_pos = true;
		}

		// A negative change in thrust will increase saturation
		_saturation_status.flags.thrust_neg = true;
	}

	if (clipping_low_yaw) {
		// check if the yaw input is saturating
		if (_rotors[index].yaw_scale > 0.0f) {
			// A negative change in yaw will increase saturation
			_saturation_status.flags.yaw_neg = true;

		} else if (_rotors[index].yaw_scale < 0.0f) {
			// A positive change in yaw will increase saturation
			_saturation_status.flags.yaw_pos = true;
		}
	}

	_saturation_status.flags.valid = true;
}

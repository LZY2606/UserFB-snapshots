/**
 *
 * "A Collection of Useful C++ Classes for Digital Signal Processing"
 * By Vinnie Falco and Bernd Porr
 *
 * Official project location:
 * https://github.com/berndporr/iir1
 *
 * See Documentation.cpp for contact information, notes, and bibliography.
 * 
 * -----------------------------------------------------------------
 *
 * License: MIT License (http://www.opensource.org/licenses/mit-license.php)
 * Copyright (c) 2009 by Vinnie Falco
 * Copyright (c) 2011 by Bernd Porr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 **/

#pragma once

#include "Common.h"
#include "Biquad.h"


#define DEFAULT_STATE DirectFormII

namespace Iir {

/**
 * Helper for the resetToConstant() methods of the state topologies.
 * Validates the constant input and the coefficients of a biquad
 * section and calculates the DC numerator (b0+b1+b2) and denominator
 * (1+a1+a2) from the coefficients as they are actually stored in the
 * Biquad (i.e. normalised by a0). Throws std::invalid_argument if the
 * input or any coefficient is not finite or if the denominator is
 * exactly zero. A tiny but non-zero denominator is accepted.
 **/
	inline void constantStateParams (const double input, const Biquad& s,
					 double& num, double& den)
	{
		if (!std::isfinite(input))
			throw_invalid_argument("resetToConstant: input is not finite.");
		if (!std::isfinite(s.m_a0) ||
		    !std::isfinite(s.m_a1) ||
		    !std::isfinite(s.m_a2) ||
		    !std::isfinite(s.m_b0) ||
		    !std::isfinite(s.m_b1) ||
		    !std::isfinite(s.m_b2))
			throw_invalid_argument("resetToConstant: coefficient is not finite.");
		num = s.m_b0 + s.m_b1 + s.m_b2;
		den = 1.0 + s.m_a1 + s.m_a2;
		if (den == 0.0)
			throw_invalid_argument("resetToConstant: 1 + a1 + a2 is zero.");
	}

/**
 * State for applying a second order section to a sample using Direct Form I
 *
 * Difference equation:
 *
 *  y[n] = (b0/a0)*x[n] + (b1/a0)*x[n-1] + (b2/a0)*x[n-2]
 *                      - (a1/a0)*y[n-1] - (a2/a0)*y[n-2]  
 **/
	class IIR_EXPORT DirectFormI
	{
	public:
	DirectFormI () = default;
	
	void reset ()
	{
		m_x1 = 0;
		m_x2 = 0;
		m_y1 = 0;
		m_y2 = 0;
	}

	/**
	 * Sets the delay lines to the steady state corresponding to a
	 * constant input of the value \param input, assuming the section
	 * has seen this input since the infinite past. Returns the
	 * equilibrium output which the next filter(input) call will
	 * produce. Throws std::invalid_argument if the equilibrium
	 * cannot be represented by finite doubles. On failure the delay
	 * lines are left untouched.
	 **/
	double resetToConstant (const double input, const Biquad& s)
	{
		double num, den;
		constantStateParams (input, s, num, den);
		const double out = input * num / den;
		if (!std::isfinite(out))
			throw_invalid_argument("resetToConstant: equilibrium is not finite.");
		m_x1 = input;
		m_x2 = input;
		m_y1 = out;
		m_y2 = out;
		return out;
	}

	inline double filter(const double in,
			     const Biquad& s)
	{
		const double out = s.m_b0*in + s.m_b1*m_x1 + s.m_b2*m_x2
		- s.m_a1*m_y1 - s.m_a2*m_y2;
		m_x2 = m_x1;
		m_y2 = m_y1;
		m_x1 = in;
		m_y1 = out;
		
		return out;
	}
	
	protected:
	double m_x2 = 0.0; // x[n-2]
	double m_y2 = 0.0; // y[n-2]
	double m_x1 = 0.0; // x[n-1]
	double m_y1 = 0.0; // y[n-1]
	};

//------------------------------------------------------------------------------

/**
 * State for applying a second order section to a sample using Direct Form II
 *
 * Difference equation:
 *
 *  v[n] =         x[n] - (a1/a0)*v[n-1] - (a2/a0)*v[n-2]
 *  y(n) = (b0/a0)*v[n] + (b1/a0)*v[n-1] + (b2/a0)*v[n-2]
 *
 **/
	class IIR_EXPORT DirectFormII
	{
	public:
	DirectFormII () = default;
	
	void reset ()
	{
		m_v1 = 0.0;
		m_v2 = 0.0;
	}

	/**
	 * Sets the delay lines to the steady state corresponding to a
	 * constant input of the value \param input, assuming the section
	 * has seen this input since the infinite past. Returns the
	 * equilibrium output which the next filter(input) call will
	 * produce. Throws std::invalid_argument if the equilibrium
	 * cannot be represented by finite doubles. On failure the delay
	 * lines are left untouched.
	 **/
	double resetToConstant (const double input, const Biquad& s)
	{
		double num, den;
		constantStateParams (input, s, num, den);
		const double v = input / den;
		const double out = num * v;
		if (!std::isfinite(v) || !std::isfinite(out))
			throw_invalid_argument("resetToConstant: equilibrium is not finite.");
		m_v1 = v;
		m_v2 = v;
		return out;
	}

	inline double filter(const double in,
			     const Biquad& s)
	{
		const double w   = in - s.m_a1*m_v1 - s.m_a2*m_v2;
		const double out =      s.m_b0*w    + s.m_b1*m_v1 + s.m_b2*m_v2;
		
		m_v2 = m_v1;
		m_v1 = w;
		
		return out;
	}
	
	private:
	double m_v1 = 0.0; // v[-1]
	double m_v2 = 0.0; // v[-2]
	};


//------------------------------------------------------------------------------
	
	class IIR_EXPORT TransposedDirectFormII
	{
	public:
	TransposedDirectFormII() = default;
	
	void reset ()
	{
		m_s1 = 0.0;
		m_s1_1 = 0.0;
		m_s2 = 0.0;
		m_s2_1 = 0.0;
	}

	/**
	 * Sets the delay lines to the steady state corresponding to a
	 * constant input of the value \param input, assuming the section
	 * has seen this input since the infinite past. Returns the
	 * equilibrium output which the next filter(input) call will
	 * produce. Throws std::invalid_argument if the equilibrium
	 * cannot be represented by finite doubles. On failure the delay
	 * lines are left untouched.
	 **/
	double resetToConstant (const double input, const Biquad& s)
	{
		double num, den;
		constantStateParams (input, s, num, den);
		const double out = input * num / den;
		const double s1 = out - s.m_b0*input;
		const double s2 = s.m_b2*input - s.m_a2*out;
		if (!std::isfinite(out) || !std::isfinite(s1) || !std::isfinite(s2))
			throw_invalid_argument("resetToConstant: equilibrium is not finite.");
		m_s1 = s1;
		m_s1_1 = s1;
		m_s2 = s2;
		m_s2_1 = s2;
		return out;
	}

	inline double filter(const double in,
			     const Biquad& s)
	{
		const double out = m_s1_1 + s.m_b0*in;
		m_s1 = m_s2_1 + s.m_b1*in - s.m_a1*out;
		m_s2 = s.m_b2*in - s.m_a2*out;
		m_s1_1 = m_s1;
		m_s2_1 = m_s2;
		
		return out;
	}
	
	private:
	double m_s1 = 0.0;
	double m_s1_1 = 0.0;
	double m_s2 = 0.0;
	double m_s2_1 = 0.0;
	};

}

/*
   Copyright (c) 2009-2016, Jack Poulson
   All rights reserved.

   This file is part of Elemental and is under the BSD 2-Clause License, 
   which can be found in the LICENSE file in the root directory, or at 
   http://opensource.org/licenses/BSD-2-Clause
*/
#ifndef EL_BLAS_SAFE_SCALE_HPP
#define EL_BLAS_SAFE_SCALE_HPP

namespace El {

template<typename Real,typename=EnableIf<IsReal<Real>>>
bool SafeScaleStep
(       Real& numerator,
        Real& denominator,
        Real& alpha,
  const Real& zero,
  const Real& smallNum,
  const Real& bigNum )
{
    DEBUG_CSE

    Real shrunkDenominator = denominator * smallNum;
    if( Abs(shrunkDenominator) > Abs(numerator) && numerator != zero )
    {
        alpha = smallNum;
        denominator = shrunkDenominator;
        return false;
    }

    Real shrunkNumerator = numerator / bigNum; 
    if( Abs(shrunkNumerator) > Abs(denominator) )
    {
        alpha = bigNum;
        numerator = shrunkNumerator;
        return false;
    }

    alpha = numerator / denominator;
    return true;
}

template<typename F>
void SafeScale( Base<F> numerator, Base<F> denominator, Matrix<F>& A )
{
    DEBUG_CSE
    typedef Base<F> Real;
    const Real zero(0);
    const Real smallNum = limits::SafeMin<Real>();
    const Real bigNum = Real(1) / smallNum;

    bool done = false;
    Real alpha;
    while( !done )
    {
        done =
          SafeScaleStep
          ( numerator, denominator, alpha, zero, smallNum, bigNum );
        A *= alpha;
    }
}

template<typename F>
void SafeScale
( Base<F> numerator, Base<F> denominator, AbstractDistMatrix<F>& A )
{
    DEBUG_CSE
    SafeScale( numerator, denominator, A.Matrix() );
}

template<typename F>
void SafeScale( Base<F> numerator, Base<F> denominator, SparseMatrix<F>& A )
{
    DEBUG_CSE
    typedef Base<F> Real;
    const Real zero(0);
    const Real smallNum = limits::SafeMin<Real>();
    const Real bigNum = Real(1) / smallNum;

    bool done = false;
    Real alpha;
    while( !done )
    {
        done =
          SafeScaleStep
          ( numerator, denominator, alpha, zero, smallNum, bigNum );
        A *= alpha;
    }
}

template<typename F>
void SafeScale( Base<F> numerator, Base<F> denominator, DistSparseMatrix<F>& A )
{
    DEBUG_CSE
    typedef Base<F> Real;
    const Real zero(0);
    const Real smallNum = limits::SafeMin<Real>();
    const Real bigNum = Real(1) / smallNum;

    bool done = false;
    Real alpha;
    while( !done )
    {
        done =
          SafeScaleStep
          ( numerator, denominator, alpha, zero, smallNum, bigNum );
        A *= alpha;
    }
}

template<typename F>
void SafeScale( Base<F> numerator, Base<F> denominator, DistMultiVec<F>& A )
{
    DEBUG_CSE
    SafeScale( numerator, denominator, A.Matrix() );
}

template<typename F>
void SafeScaleHermitianTridiagonal
( Base<F> numerator, Base<F> denominator, Matrix<Base<F>>& d, Matrix<F>& e )
{
    DEBUG_CSE
    typedef Base<F> Real;
    const Real zero(0);
    const Real smallNum = limits::SafeMin<Real>();
    const Real bigNum = Real(1) / smallNum;

    bool done = false;
    Real alpha;
    while( !done )
    {
        done =
          SafeScaleStep
          ( numerator, denominator, alpha, zero, smallNum, bigNum );
        d *= alpha;
        e *= alpha;
    }
}

} // namespace El

#endif // ifndef EL_BLAS_SAFE_SCALE_HPP

/*
   Copyright (c) 2009-2014, Jack Poulson
   All rights reserved.

   This file is part of Elemental and is under the BSD 2-Clause License, 
   which can be found in the LICENSE file in the root directory, or at 
   http://opensource.org/licenses/BSD-2-Clause
*/
#pragma once
#ifndef ELEM_MULTISHIFTHESSSOLVE_HPP
#define ELEM_MULTISHIFTHESSSOLVE_HPP

// NOTE: These algorithms are adaptations and/or extensions of Alg. 2 from
//       Greg Henry's "The shifted Hessenberg system solve computation".
//       It is important to note that the Given's rotation definition in
//       said paper is the adjoint of the LAPACK definition (as well as 
//       leaving out a conjugation necessary for the complex case).

namespace elem {
namespace mshs {

template<typename F>
inline void
LN( F alpha, const Matrix<F>& H, const Matrix<F>& shifts, Matrix<F>& X ) 
{
    DEBUG_ONLY(CallStackEntry cse("mshs::LN"))
    Scale( alpha, X );

    const Int m = X.Height();
    const Int n = X.Width();
    if( m == 0 )
        return;

    // Initialize storage for Givens rotations
    typedef Base<F> Real;
    Matrix<Real> C(m,n);
    Matrix<F> S(m,n);

    // Initialize the workspace for shifted columns of H
    Matrix<F> W(m,n);
    for( Int j=0; j<n; ++j )
    {
        MemCopy( W.Buffer(0,j), H.LockedBuffer(), m );
        W.Update( 0, j, -shifts.Get(j,0) );
    }
     
    // Simultaneously find the LQ factorization and solve against L
    for( Int k=0; k<m-1; ++k )
    {
        auto hB = LockedViewRange( H, k+2, k+1, m, k+2 );
        const F etakkp1 = H.Get(k,k+1);
        const F etakp1kp1 = H.Get(k+1,k+1);
        for( Int j=0; j<n; ++j )
        {
            // Find the Givens rotation needed to zero H(k,k+1),
            //   | c        s | | H(k,k)   | = | gamma |
            //   | -conj(s) c | | H(k,k+1) |   | 0     |
            Real c;
            F s, rho;
            lapack::ComputeGivens( W.Get(k,j), etakkp1, &c, &s, &rho );
            C.Set( k, j, c );
            S.Set( k, j, s );

            // The new diagonal value of L
            const F lambdakk = c*W.Get(k,j) + s*etakkp1;

            // Divide our current entry of x by the diagonal value of L
            X.Set( k, j, X.Get(k,j)/lambdakk );

            // x(k+1:end) -= x(k) * L(k+1:end,k), where
            // L(k+1:end,k) = c H(k+1:end,k) + s H(k+1:end,k+1). We express this
            // more concisely as xB -= x(k) * ( c wB + s hB ).
            // Note that we carefully handle updating the k+1'th entry since
            // it is shift-dependent.
            const F mu = shifts.Get( j, 0 );
            const F xc = X.Get(k,j)*c;
            const F xs  = X.Get(k,j)*s;
            X.Update( k+1, j, -xc*W.Get(k+1,j)-xs*(etakp1kp1-mu) );
            auto xB = ViewRange( X, k+2, j, m, j+1 );
            auto wB = ViewRange( W, k+2, j, m, j+1 );
            Axpy( -xc, wB, xB );
            Axpy( -xs, hB, xB );

            // Change the working vector, wB, from representing a fully-updated
            // portion of the k'th column of H from the end of the last 
            // to a fully-updated portion of the k+1'th column of this iteration
            //
            // w(k+1:end) := -conj(s) H(k+1:end,k) + c H(k+1:end,k+1)
            W.Set( k+1, j, -Conj(s)*W.Get(k+1,j)+c*(etakp1kp1-mu) );
            Scale( -Conj(s), wB );
            Axpy( c, hB, wB );
        }
    }
    // Divide x(end) by L(end,end)
    for( Int j=0; j<n; ++j )
        X.Set( m-1, j, X.Get(m-1,j)/W.Get(m-1,j) );

    // Solve against Q
    Matrix<F> t1(n,1), t2(n,1);
    for( Int j=0; j<n; ++j )
        t1.Set( j, 0, X.Get(m-1,j) );
    for( Int k=m-2; k>=0; --k )
    {
        for( Int j=0; j<n; ++j )        
        {
            t2.Set( j,   0, X.Get(k,j)                                    );
            X.Set(  k+1, j, C.Get(k,j)*t1.Get(j,0)+S.Get(k,j)*t2.Get(j,0) );
            t1.Set( j,   0,      C.Get(k,j) *t2.Get(j,0)-
                            Conj(S.Get(k,j))*t1.Get(j,0) );
        }
    }
    for( Int j=0; j<n; ++j )
        X.Set( 0, j, t1.Get(j,0) );
}

template<typename F>
inline void
UN( F alpha, const Matrix<F>& H, const Matrix<F>& shifts, Matrix<F>& X ) 
{
    DEBUG_ONLY(CallStackEntry cse("mshs::UN"))
    Scale( alpha, X );

    const Int m = X.Height();
    const Int n = X.Width();
    if( m == 0 )
        return;

    // Initialize storage for Givens rotations
    typedef Base<F> Real;
    Matrix<Real> C(m,n);
    Matrix<F> S(m,n);

    // Initialize the workspace for shifted columns of H
    Matrix<F> W(m,n);
    for( Int j=0; j<n; ++j )
    {
        MemCopy( W.Buffer(0,j), H.LockedBuffer(0,m-1), m );
        W.Update( m-1, j, -shifts.Get(j,0) );
    }
     
    // Simultaneously form the RQ factorization and solve against R
    for( Int k=m-1; k>0; --k )
    {
        auto hT = LockedView( H, 0, k-1, k-1, 1 );
        const F etakkm1 = H.Get(k,k-1);
        const F etakm1km1 = H.Get(k-1,k-1);
        for( Int j=0; j<n; ++j )
        {
            // Find the Givens rotation needed to zero H(k,k-1),
            //   | c        s | | H(k,k)   | = | gamma |
            //   | -conj(s) c | | H(k,k-1) |   | 0     |
            Real c;
            F s, gamma;
            lapack::ComputeGivens( W.Get(k,j), etakkm1, &c, &s, &gamma );
            C.Set( k, j, c );
            S.Set( k, j, s );

            // The new diagonal value of R
            const F rhokk = c*W.Get(k,j) + s*etakkm1;

            // Divide our current entry of x by the diagonal value of R
            X.Set( k, j, X.Get(k,j)/rhokk );

            // x(0:k-1) -= x(k) * R(0:k-1,k), where
            // R(0:k-1,k) = c H(0:k-1,k) + s H(0:k-1,k-1). We express this
            // more concisely as xT -= x(k) * ( c wT + s hT ).
            // Note that we carefully handle updating the k-1'th entry since
            // it is shift-dependent.
            const F mu = shifts.Get( j, 0 );
            const F xc = X.Get(k,j)*c;
            const F xs  = X.Get(k,j)*s;
            auto xT = View( X, 0, j, k-1, 1 );
            auto wT = View( W, 0, j, k-1, 1 );
            Axpy( -xc, wT, xT );
            Axpy( -xs, hT, xT );
            X.Update( k-1, j, -xc*W.Get(k-1,j)-xs*(etakm1km1-mu) );

            // Change the working vector, wT, from representing a fully-updated
            // portion of the k'th column of H from the end of the last 
            // to a fully-updated portion of the k-1'th column of this iteration
            //
            // w(0:k-1) := -conj(s) H(0:k-1,k) + c H(0:k-1,k-1)
            Scale( -Conj(s), wT );
            Axpy( c, hT, wT );
            W.Set( k-1, j, -Conj(s)*W.Get(k-1,j)+c*(etakm1km1-mu) );
        }
    }
    // Divide x(0) by R(0,0)
    for( Int j=0; j<n; ++j )
        X.Set( 0, j, X.Get(0,j)/W.Get(0,j) );

    // Solve against Q
    Matrix<F> t1(n,1), t2(n,1);
    for( Int j=0; j<n; ++j )
        t1.Set( j, 0, X.Get(0,j) );
    for( Int k=1; k<m; ++k )
    {
        for( Int j=0; j<n; ++j )        
        {
            t2.Set( j,   0, X.Get(k,j)                                    );
            X.Set(  k-1, j, C.Get(k,j)*t1.Get(j,0)+S.Get(k,j)*t2.Get(j,0) );
            t1.Set( j,   0,      C.Get(k,j) *t2.Get(j,0)-
                            Conj(S.Get(k,j))*t1.Get(j,0) );
        }
    }
    for( Int j=0; j<n; ++j )
        X.Set( m-1, j, t1.Get(j,0) );
}

// NOTE: A [VC,* ] distribution might be most appropriate for the 
//       Hessenberg matrices since whole columns will need to be formed 
//       on every process and this distribution will keep the communication 
//       balanced.

template<typename F,Dist UH,Dist VH,Dist VX>
inline void
LN
( F alpha, const DistMatrix<F,UH,VH>& H, const DistMatrix<F,VX,STAR>& shifts,
  DistMatrix<F,STAR,VX>& X ) 
{
    DEBUG_ONLY(
        CallStackEntry cse("mshs::LN");
        if( shifts.ColAlign() != X.RowAlign() )
            LogicError("shifts and X are not aligned");
    )
    Scale( alpha, X );

    const Int m = X.Height();
    const Int n = X.Width();
    const Int nLoc = X.LocalWidth();
    if( m == 0 )
        return;

    // Initialize storage for Givens rotations
    typedef Base<F> Real;
    Matrix<Real> C(m,nLoc);
    Matrix<F> S(m,nLoc);

    // Initialize the workspace for shifted columns of H
    Matrix<F> W(m,nLoc);
    {
        auto h0 = LockedView( H, 0, 0, m, 1 );
        DistMatrix<F,STAR,STAR> h0_STAR_STAR( h0 );
        for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        {
            MemCopy( W.Buffer(0,jLoc), h0_STAR_STAR.LockedBuffer(), m );
            W.Update( 0, jLoc, -shifts.GetLocal(jLoc,0) );
        }
    }
     
    // Simultaneously find the LQ factorization and solve against L
    DistMatrix<F,STAR,STAR> hB_STAR_STAR( H.Grid() );
    for( Int k=0; k<m-1; ++k )
    {
        auto hB = LockedViewRange( H, k+2, k+1, m, k+2 );
        hB_STAR_STAR = hB;
        const F etakkp1 = H.Get(k,k+1);
        const F etakp1kp1 = H.Get(k+1,k+1);
        for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        {
            // Find the Givens rotation needed to zero H(k,k+1),
            //   | c        s | | H(k,k)   | = | gamma |
            //   | -conj(s) c | | H(k,k+1) |   | 0     |
            Real c;
            F s, rho;
            lapack::ComputeGivens( W.Get(k,jLoc), etakkp1, &c, &s, &rho );
            C.Set( k, jLoc, c );
            S.Set( k, jLoc, s );

            // The new diagonal value of L
            const F lambdakk = c*W.Get(k,jLoc) + s*etakkp1;

            // Divide our current entry of x by the diagonal value of L
            X.SetLocal( k, jLoc, X.GetLocal(k,jLoc)/lambdakk );

            // x(k+1:end) -= x(k) * L(k+1:end,k), where
            // L(k+1:end,k) = c H(k+1:end,k) + s H(k+1:end,k+1). We express this
            // more concisely as xB -= x(k) * ( c wB + s hB ).
            // Note that we carefully handle updating the k+1'th entry since
            // it is shift-dependent.
            const F mu = shifts.GetLocal( jLoc, 0 );
            const F xc = X.GetLocal(k,jLoc)*c;
            const F xs  = X.GetLocal(k,jLoc)*s;
            X.UpdateLocal( k+1, jLoc, -xc*W.Get(k+1,jLoc)-xs*(etakp1kp1-mu) );
            auto xB = ViewRange( X.Matrix(), k+2, jLoc, m, jLoc+1 );
            auto wB = ViewRange( W,          k+2, jLoc, m, jLoc+1 );
            Axpy( -xc, wB, xB );
            Axpy( -xs, hB_STAR_STAR.LockedMatrix(), xB );

            // Change the working vector, wB, from representing a fully-updated
            // portion of the k'th column of H from the end of the last 
            // to a fully-updated portion of the k+1'th column of this iteration
            //
            // w(k+1:end) := -conj(s) H(k+1:end,k) + c H(k+1:end,k+1)
            W.Set( k+1, jLoc, -Conj(s)*W.Get(k+1,jLoc)+c*(etakp1kp1-mu) );
            Scale( -Conj(s), wB );
            Axpy( c, hB_STAR_STAR.LockedMatrix(), wB );
        }
    }
    // Divide x(end) by L(end,end)
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        X.SetLocal( m-1, jLoc, X.GetLocal(m-1,jLoc)/W.Get(m-1,jLoc) );

    // Solve against Q
    Matrix<F> t1(nLoc,1), t2(nLoc,1);
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        t1.Set( jLoc, 0, X.GetLocal(m-1,jLoc) );
    for( Int k=m-2; k>=0; --k )
    {
        for( Int jLoc=0; jLoc<nLoc; ++jLoc ) 
        {
            t2.Set( jLoc, 0, X.GetLocal(k,jLoc) );
            X.SetLocal( k+1, jLoc, C.Get(k,jLoc)*t1.Get(jLoc,0)+
                                   S.Get(k,jLoc)*t2.Get(jLoc,0) );
            t1.Set( jLoc, 0,      C.Get(k,jLoc) *t2.Get(jLoc,0)-
                             Conj(S.Get(k,jLoc))*t1.Get(jLoc,0) );
        }
    }
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        X.SetLocal( 0, jLoc, t1.Get(jLoc,0) );
}

template<typename F,Dist UH,Dist VH,Dist VX>
inline void
UN
( F alpha, const DistMatrix<F,UH,VH>& H, const DistMatrix<F,VX,STAR>& shifts,
  DistMatrix<F,STAR,VX>& X ) 
{
    DEBUG_ONLY(
        CallStackEntry cse("mshs::UN");
        if( shifts.ColAlign() != X.RowAlign() )
            LogicError("shifts and X are not aligned");
    )
    Scale( alpha, X );

    const Int m = X.Height();
    const Int n = X.Width();
    const Int nLoc = X.LocalWidth();
    if( m == 0 )
        return;

    // Initialize storage for Givens rotations
    typedef Base<F> Real;
    Matrix<Real> C(m,nLoc);
    Matrix<F> S(m,nLoc);

    // Initialize the workspace for shifted columns of H
    Matrix<F> W(m,nLoc);
    {
        auto hLast = LockedView( H, 0, m-1, m, 1 );
        DistMatrix<F,STAR,STAR> hLast_STAR_STAR( hLast );
        for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        {
            MemCopy( W.Buffer(0,jLoc), hLast_STAR_STAR.LockedBuffer(), m );
            W.Update( m-1, jLoc, -shifts.GetLocal(jLoc,0) );
        }
    }
     
    // Simultaneously form the RQ factorization and solve against R
    DistMatrix<F,STAR,STAR> hT_STAR_STAR( H.Grid() );
    for( Int k=m-1; k>0; --k )
    {
        auto hT = LockedView( H, 0, k-1, k-1, 1 );
        hT_STAR_STAR = hT;
        const F etakkm1 = H.Get(k,k-1);
        const F etakm1km1 = H.Get(k-1,k-1);
        for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        {
            // Find the Givens rotation needed to zero H(k,k-1),
            //   | c        s | | H(k,k)   | = | gamma |
            //   | -conj(s) c | | H(k,k-1) |   | 0     |
            Real c;
            F s, gamma;
            lapack::ComputeGivens( W.Get(k,jLoc), etakkm1, &c, &s, &gamma );
            C.Set( k, jLoc, c );
            S.Set( k, jLoc, s );

            // The new diagonal value of R
            const F rhokk = c*W.Get(k,jLoc) + s*etakkm1;

            // Divide our current entry of x by the diagonal value of R
            X.SetLocal( k, jLoc, X.GetLocal(k,jLoc)/rhokk );

            // x(0:k-1) -= x(k) * R(0:k-1,k), where
            // R(0:k-1,k) = c H(0:k-1,k) + s H(0:k-1,k-1). We express this
            // more concisely as xT -= x(k) * ( c wT + s hT ).
            // Note that we carefully handle updating the k-1'th entry since
            // it is shift-dependent.
            const F mu = shifts.GetLocal( jLoc, 0 );
            const F xc = X.GetLocal(k,jLoc)*c;
            const F xs  = X.GetLocal(k,jLoc)*s;
            auto xT = View( X.Matrix(), 0, jLoc, k-1, 1 );
            auto wT = View( W,          0, jLoc, k-1, 1 );
            Axpy( -xc, wT, xT );
            Axpy( -xs, hT_STAR_STAR.LockedMatrix(), xT );
            X.UpdateLocal( k-1, jLoc, -xc*W.Get(k-1,jLoc)-xs*(etakm1km1-mu) );

            // Change the working vector, wT, from representing a fully-updated
            // portion of the k'th column of H from the end of the last 
            // to a fully-updated portion of the k-1'th column of this iteration
            //
            // w(0:k-1) := -conj(s) H(0:k-1,k) + c H(0:k-1,k-1)
            Scale( -Conj(s), wT );
            Axpy( c, hT_STAR_STAR.LockedMatrix(), wT );
            W.Set( k-1, jLoc, -Conj(s)*W.Get(k-1,jLoc)+c*(etakm1km1-mu) );
        }
    }
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        X.SetLocal( 0, jLoc, X.GetLocal(0,jLoc)/W.Get(0,jLoc) );

    // Solve against Q
    Matrix<F> t1(nLoc,1), t2(nLoc,1);
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        t1.Set( jLoc, 0, X.GetLocal(0,jLoc) );
    for( Int k=1; k<m; ++k )
    {
        for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        {
            t2.Set( jLoc, 0, X.GetLocal(k,jLoc) );
            X.SetLocal( k-1, jLoc, C.Get(k,jLoc)*t1.Get(jLoc,0)+
                                   S.Get(k,jLoc)*t2.Get(jLoc,0) );
            t1.Set( jLoc, 0,      C.Get(k,jLoc) *t2.Get(jLoc,0)-
                             Conj(S.Get(k,jLoc))*t1.Get(jLoc,0) );
        }
    }
    for( Int jLoc=0; jLoc<nLoc; ++jLoc )
        X.SetLocal( m-1, jLoc, t1.Get(jLoc,0) );
}

// TODO: UT and LT

} // namespace mshs

template<typename F>
inline void
MultiShiftHessSolve
( UpperOrLower uplo, Orientation orientation,
  F alpha, const Matrix<F>& H, const Matrix<F>& shifts, Matrix<F>& X )
{
    DEBUG_ONLY(CallStackEntry cse("MultiShiftHessSolve"))
    if( uplo == LOWER )
    {
        if( orientation == NORMAL )
            mshs::LN( alpha, H, shifts, X );
        else
            LogicError("This option is not yet supported");
    }
    else
    {
        if( orientation == NORMAL )
            mshs::UN( alpha, H, shifts, X );
        else
            LogicError("This option is not yet supported");
    }
}

template<typename F,Dist UH,Dist VH,Dist VX>
inline void
MultiShiftHessSolve
( UpperOrLower uplo, Orientation orientation,
  F alpha, const DistMatrix<F,UH,VH>& H, const DistMatrix<F,VX,STAR>& shifts, 
  DistMatrix<F,STAR,VX>& X )
{
    DEBUG_ONLY(CallStackEntry cse("MultiShiftHessSolve"))
    if( uplo == LOWER )
    {
        if( orientation == NORMAL )
            mshs::LN( alpha, H, shifts, X );
        else
            LogicError("This option is not yet supported");
    }
    else
    {
        if( orientation == NORMAL )
            mshs::UN( alpha, H, shifts, X );
        else
            LogicError("This option is not yet supported");
    }
}

} // namespace elem

#endif // ifndef ELEM_MULTISHIFTHESSSOLVE_HPP
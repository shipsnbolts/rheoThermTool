/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright held by original author
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

\*---------------------------------------------------------------------------*/

#include "Giesekus.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
namespace constitutiveEqs 
{
    defineTypeNameAndDebug(Giesekus, 0);
    addToRunTimeSelectionTable(constitutiveEq, Giesekus, dictionary);
}
}

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::constitutiveEqs::Giesekus::Giesekus
(
    const word& name,
    const volVectorField& U,
    const surfaceScalarField& phi,
    const volScalarField& T,
    const dictionary& dict
)
:
    constitutiveEq(name, U, phi, T),
    tau_
    (
        IOobject
        (
            "tau" + name,
            U.time().timeName(),
            U.mesh(),
            IOobject::MUST_READ,
            IOobject::AUTO_WRITE
        ),
        U.mesh()
    ),
    elastEnergDiss_(dict.lookup("elastEnergDiss")),
    alpha_(dict.lookup("alpha")),
    lambda_
    (
        IOobject
        (
            "lambda" + name,
            U.time().timeName(),
            U.mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        U.mesh(),
        dimensionedScalar(dict.lookup("lambda"))
    )
{
    checkForStab(dict);

    rhoRef().rename("rho" + name);
    rhoRef().readOpt() = IOobject::READ_IF_PRESENT;
    rhoRef().writeOpt() = IOobject::AUTO_WRITE;
    rhoRef() = volScalarField
    (
        IOobject
        (
            "rho" + name,
            U.time().timeName(),
            U.mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        U.mesh(),
        dimensionedScalar(dict.lookup("rho"))
    );

    etaSRef().rename("etaS" + name);
    etaSRef().readOpt() = IOobject::READ_IF_PRESENT;
    etaSRef().writeOpt() = IOobject::AUTO_WRITE;
    etaSRef() = volScalarField
    (
        IOobject
        (
            "etaS" + name,
            U.time().timeName(),
            U.mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        U.mesh(),
        dimensionedScalar(dict.lookup("etaS"))
    );

    etaPRef().rename("etaP" + name);
    etaPRef().readOpt() = IOobject::READ_IF_PRESENT;
    etaPRef().writeOpt() = IOobject::AUTO_WRITE;
    etaPRef() = volScalarField
    (
        IOobject
        (
            "etaP" + name,
            U.time().timeName(),
            U.mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        U.mesh(),
        dimensionedScalar(dict.lookup("etaP"))
    );
}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

Foam::tmp<Foam::volScalarField> Foam::constitutiveEqs::Giesekus::dotTHfun()
{
    //- Get dimensionless ln(T) field to compute DlnT/Dt
    //  WARNING: This may yield problems if the ln(T) field is not
    //  a properly build with an IOobject due to the time derivative!
    //dimensionedScalar Tone (word::null, T().dimensions(), 1); 
    //volScalarField lnT ("lnT", Foam::log(T/Tone));

    // Compute DT/Dt
    //volScalarField DTDt = fvc::ddt(T())
    //    + fvc::div(phi(),T()) - T()*fvc::div(phi());
    volScalarField DTDt = fvc::DDt(phi(), T());

    return tmp<volScalarField>
    (
        new volScalarField
        (   
            "dotTHfun", elastEnergDiss_/T()*DTDt - fvc::div(phi())
        )
    );
}

void Foam::constitutiveEqs::Giesekus::correct()
{
    dimensionedSymmTensor Itensor
    (
        "Identity", dimensionSet(0, 0, 0, 0, 0, 0, 0), symmTensor::I
    );

    // Velocity gradient tensor
    volTensorField L = fvc::grad(U());

    // Convected derivate term
    volTensorField C = tau_ & L;

    // Twice the rate of deformation tensor
    volSymmTensorField twoD = twoSymm(L);

    // Compute dotLambda = Dlambda/Dt
    //volScalarField dotLambda = fvc::ddt(lambda_)
    //    + fvc::div(phi(),lambda_) - lambda_*fvc::div(phi());
    volScalarField dotLambda = fvc::DDt(phi(), lambda_);

    // Compute dotEtaP = DetaP/Dt
    //volScalarField dotEtaP = fvc::ddt(etaPRef())
    //    + fvc::div(phi(),etaPRef()) - etaPRef()*fvc::div(phi());
    volScalarField dotEtaP = fvc::DDt(phi(), etaPRef());

    // Stress transport equation
    fvSymmTensorMatrix tauEqn
    (
        fvm::ddt(tau_)
      + fvm::div(phi(), tau_)
     ==
        etaP()/lambda_*twoD
      + twoSymm(C)
      - (alpha_/etaP())*(symm(tau_ & tau_))
      - fvm::Sp(1.0/lambda_, tau_)
      // Thermal dependency dotT*Ht
      - fvm::SuSp(-dotTHfun(), tau_)  // Thermal dependency dotT*H*tau_
      + dotTHfun()*etaP()/lambda_*Itensor  // Thermal dependency dotT*H*G*I
      // The following term appears when the constitutive equation is formulated
      // in terms of deviatoric tau tensor instead of the Cauchy sigma tensor,
      // when expanding the upper-convected derivative of sigma.
      // It yields zero if the thermal functions of lambda and etaP are the same.
      + (etaP()/lambda_*dotLambda - dotEtaP)/lambda_*Itensor
    );

    tauEqn.relax();
    tauEqn.solve();
}

Foam::tmp<Foam::volScalarField> Foam::constitutiveEqs::Giesekus::energyExtraTerms()
{
    return tmp<volScalarField>
    (
        new volScalarField
        (
            "energyExtraTerms",
            (
                elastEnergDiss_ * (
                    //(tau_ && symm(fvc::grad(U())))  // can be simplified...
                    (tau_ && fvc::grad(U()))  // T:S==T:grad(U) for symmetric T
                  + etaP()/lambda_*fvc::div(phi())
                )
              + (1.0 - elastEnergDiss_) * 0.5 * (
	            alpha_/etaP() * (tau_ && tau_)
                  + tr(tau_)/lambda_
                )
            )
        )
    );
}

// ************************************************************************* //

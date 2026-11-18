// Decompiled from the car physics library: FINITCAR.c, FPHYSLIB.c, FRESPONSE.c, FUPDATE.c, F3DMATH.c, FFORCES.c, FBOAT.c.
//
// Everything is floats on the PS2's fixed-point scale: 4096 is one, so products are divided by 4096 (kS). Forces are
// accumulated as impulses on each car, then FUpdateCarBody turns them into velocity and position. 0.59375 is the
// physics timestep. Each wheel is also a collision sphere: the first four spheres are the wheels.

#include "ai_fuzz.h"
#include "physics_fuzz.h"
#include "ee_math.h"

#define F(value) ::rc::ee_float(value##f)

namespace rc {

uint32 PS2C_Vec16Length(ptr32<_sVec16> pVec);
void FMatNormalise(ptr32<_sFMat> pMatOut, ptr32<_sFMat> pMatIn);

namespace {

const ee_float kS = F(0.00024414062); // 1 / 4096.
const ee_float kTimestep = F(0.59375);

ee_float rowMul(ee_float a, ee_float b, ee_float c, ee_float x, ee_float y, ee_float z)
{
    return (a * x + b * y + c * z) * kS;
}

sFVec matMul(const sFMat &m, const sFVec &v)
{
    sFVec out;
    out.x = rowMul(m.Xx, m.Xy, m.Xz, v.x, v.y, v.z);
    out.y = rowMul(m.Yx, m.Yy, m.Yz, v.x, v.y, v.z);
    out.z = rowMul(m.Zx, m.Zy, m.Zz, v.x, v.y, v.z);
    return out;
}

void addTorque(_sFPhysLibCar &car, const sFVec &force, const sFVec &offset)
{
    car.angImpulse.x = car.angImpulse.x + (force.y * offset.z - force.z * offset.y) * kS;
    car.angImpulse.y = car.angImpulse.y + (force.z * offset.x - force.x * offset.z) * kS;
    car.angImpulse.z = car.angImpulse.z + (force.x * offset.y - force.y * offset.x) * kS;
}

void addScaledTorque(_sFPhysLibCar &car, const sFVec &force, const sFVec &offset, ee_float scale)
{
    car.angImpulse.x = car.angImpulse.x + (force.y * offset.z - force.z * offset.y) * kS * scale;
    car.angImpulse.y = car.angImpulse.y + (force.z * offset.x - force.x * offset.z) * kS * scale;
    car.angImpulse.z = car.angImpulse.z + (force.x * offset.y - force.y * offset.x) * kS * scale;
}

void addImpulse(_sFPhysLibCar &car, const sFVec &force)
{
    car.impulse.x = car.impulse.x + force.x;
    car.impulse.y = car.impulse.y + force.y;
    car.impulse.z = car.impulse.z + force.z;
}

const sColCellVecPSX &gridVector(int16 sid)
{
    return G(acolG).pGrid->pVec.get()[sid];
}

bool tooLarge(ee_float value)
{
    return F(8192.0) < ee_abs(value);
}

// The contact point of a sphere relative to the car: its centre, moved to its surface along the normal.
sFVec surfaceOffset(const _sFPhysLibCar &car, const _sFPhysLibColSphere &sphere, const sFVec &normal)
{
    const ee_float toSurface = -sphere.radius * kS;
    const sFVec centre = matMul(car.mat, sphere.sphereOffset);
    sFVec out;
    out.x = centre.x + normal.x * toSurface;
    out.y = centre.y + normal.y * toSurface;
    out.z = centre.z + normal.z * toSurface;
    return out;
}

void updateRenderPos(_sFPhysLibCar &car, const _sFPhysLibCarData &data, const sFVec &carPos)
{
    const sFVec &model = car.mode == 1 ? data.modelOffset : data.modelOffsetBoat;
    const sFVec moved = matMul(car.mat, model);
    car.renderPos.x = carPos.x + moved.x;
    car.renderPos.y = carPos.y + moved.y;
    car.renderPos.z = carPos.z + moved.z;
}

void updateOrientation(_sFMat &mat, const sFVec &angVel)
{
    if (angVel.x == F(0.0) && angVel.y == F(0.0) && angVel.z == F(0.0))
        return;

    const ee_float length = ee_sqrt(angVel.x * angVel.x + angVel.y * angVel.y + angVel.z * angVel.z);
    const ee_float unit = F(4096.0) / length;
    const ee_float halfAngle = length * F(0.5) * F(0.019897461) * float_from_bits(0x40490FDAu);
    const ee_float z = angVel.z * unit;
    const ee_float x = angVel.x * unit;
    const ee_float y = angVel.y * unit;

    const ee_float angle = (halfAngle + halfAngle) * kS;
    const ee_float sine = ee_sinf(angle) * F(4096.0);
    const ee_float cosineTerm = sine * ee_cosf(angle) * F(4096.0);
    const ee_float s2 = (sine * sine + sine * sine) * kS;
    const ee_float c2 = (cosineTerm + cosineTerm) * kS;

    const ee_float xs = x * kS * s2 * kS;
    const ee_float yz = y * kS * s2 * kS * z;
    const ee_float yy = y * y * kS * s2 * kS;
    const ee_float xz = xs * z;
    const ee_float oneMinusXX = F(4096.0) - x * x * kS * s2 * kS;
    const ee_float xy = xs * y;
    const ee_float zc = z * c2 * kS;
    const ee_float xc = x * c2 * kS;
    const ee_float yc = y * c2 * kS;
    const ee_float zz = z * z * kS * s2 * kS;

    _sFMat rotation;
    rotation.Xx = (F(4096.0) - yy) - zz;
    rotation.Xy = xy + zc;
    rotation.Xz = xz - yc;
    rotation.Yx = xy - zc;
    rotation.Yy = oneMinusXX - zz;
    rotation.Yz = yz + xc;
    rotation.Zx = xz + yc;
    rotation.Zy = yz - xc;
    rotation.Zz = oneMinusXX - yy;
    fmatMul(mat, rotation, mat);
}

} // namespace

// FINITCAR.c

int32_t FPhysLibSphereInitFixup(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    if (car.mode != 1)
        return 0;
    _sFPhysLibColSphere *sphere = car.pSphere.get();
    const _sFPhysLibCarData &data = *car.pData;
    if (G(bUsingTestSpheres) == 0)
    {
        for (int32_t i = 0; i < car.numSpheres; ++i)
        {
            sphere[i].sphereOffset.x = sphere[i].sphereOffset.x + data.modelOffset.x;
            sphere[i].sphereOffset.y = sphere[i].sphereOffset.y + data.modelOffset.y;
            sphere[i].sphereOffset.z = sphere[i].sphereOffset.z + data.modelOffset.z;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        sphere = car.pSphere.get();
        const sFVec offset = sphere[i].sphereOffset;
        car.wheel[i].suspOffset = offset;
        const ee_float spring = i < 2 ? data.frontWheels.springConstant2 : data.rearWheels.springConstant2;
        car.wheel[i].suspOffset.y = car.wheel[i].suspOffset.y - (data.mass * G(FPhysLib).gravity) / (spring * F(4.0));
    }
    return 0;
}

void FInitialiseCar(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    _sFPhysLibCarData &data = *car.pData;
    car.mode = 1;
    car.mat.Xx = 4096.0f;
    car.mat.Zz = 4096.0f;
    car.mat.Yy = 4096.0f;
    car.mat.Xz = 0.0f;
    car.mat.Xy = 0.0f;
    car.mat.Yz = 0.0f;
    car.mat.Yx = 0.0f;
    car.mat.Zy = 0.0f;
    car.mat.Zx = 0.0f;
    car.frontWheelMat = car.mat;
    car.pos.x = 0.0f;
    car.pos.y = 0.0f;
    car.pos.z = 0.0f;

    const _sFPhysLibColSphere *sphere = car.pSphere.get();
    ee_float modelX = data.modelOffset.x, modelY = data.modelOffset.y, modelZ = data.modelOffset.z;
    if (modelX == F(-1.0) && modelY == F(-1.0) && modelZ == F(-1.0))
    {
        // A model offset of -1 means "work it out": the point midway between the four wheels, less the centre of mass.
        modelX = -(sphere[0].sphereOffset.x + F(0.0) + sphere[1].sphereOffset.x + sphere[2].sphereOffset.x + sphere[3].sphereOffset.x) * F(0.25) -
                 data.comFromWheelMidPoint.x;
        data.modelOffset.x = modelX;
        modelY = -(sphere[0].sphereOffset.y + F(0.0) + sphere[1].sphereOffset.y + sphere[2].sphereOffset.y + sphere[3].sphereOffset.y) * F(0.25) -
                 data.comFromWheelMidPoint.y;
        data.modelOffset.y = modelY;
        modelZ = -(sphere[0].sphereOffset.z + F(0.0) + sphere[1].sphereOffset.z + sphere[2].sphereOffset.z + sphere[3].sphereOffset.z) * F(0.25) -
                 data.comFromWheelMidPoint.z;
        data.modelOffset.z = modelZ;
    }
    else
    {
        data.comFromWheelMidPoint.z = -1.0f;
        data.comFromWheelMidPoint.x = -1.0f;
        data.comFromWheelMidPoint.y = -1.0f;
    }

    data.modelOffsetBoat.x = modelX + data.comBoatOffset.x;
    data.invMass = F(16777216.0) / data.mass;
    data.modelOffsetBoat.y = modelY + data.comBoatOffset.y;
    data.modelOffsetBoat.z = modelZ + data.comBoatOffset.z;
    data.invInertiaX = F(16777216.0) / data.inertiaX;
    data.frontWheels.radius = sphere[0].radius;
    data.frontWheels.invMass = F(16777216.0) / data.frontWheels.mass;
    data.frontWheels.invInertia = F(16777216.0) / data.frontWheels.inertia;
    data.frontWheels.maxFrictionalForce2 = data.frontWheels.maxFrictionalForce * data.frontWheels.maxFrictionalForce;
    data.rearWheels.maxFrictionalForce2 = data.rearWheels.maxFrictionalForce * data.rearWheels.maxFrictionalForce;
    data.rearWheels.radius = sphere[2].radius;
    data.rearWheels.invMass = F(16777216.0) / data.rearWheels.mass;
    data.rearWheels.invInertia = F(16777216.0) / data.rearWheels.inertia;
    FPhysLibSphereInitFixup(pCar);
}

// A test car's handling, used before the real car data existed: every field is set, those it does not name to -1
// (all ones, as the original's memset leaves them). The original also works out the point between the wheels into a
// local it never uses.
int32_t FPhysLibInitTESTCarData(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    _sFPhysLibCarData &data = *car.pData;
    std::memset(&data, 0xff, sizeof(_sFPhysLibCarData));
    data.carExtents = {float_from_bits(0x46147000u), float_from_bits(0x45D48000u), float_from_bits(0x4684D000u)};
    data.comFromWheelMidPoint = {F(0.0), F(2000.0), F(0.0)};
    data.inertiaZ = 16384.0f;
    data.inertiaX = 16384.0f;
    data.inertiaY = 16384.0f;
    data.maxSteerAngle = 0x32;
    data.linDampConstant = 15.0f;
    data.angDampConstant = 4000.0f;
    data.frontWheels.inertia = 4096.0f;
    data.frontWheels.springConstant = 125.0f;
    data.frontWheels.suspLength = 6000.0f;
    data.frontWheels.gripConstant = 7000.0f;
    data.frontWheels.maxFrictionalForce = 150.0f;
    data.mass = 4096.0f;
    data.colFricConstant = 100.0f;
    data.frontWheels.mass = 150.0f;
    data.frontWheels.springConstant2 = 300.0f;
    data.frontWheels.springDampConstant = 100.0f;
    data.frontWheels.maxNormalForce = float_from_bits(0x4E800000u);
    data.frontWheels.maxDrivingImpulse = 480.0f;
    data.frontWheels.axleDampingConstant = 100.0f;
    data.rearWheels = data.frontWheels; // radius, invMass, invInertia and maxFrictionalForce2 are still all ones
    data.rearWheels.maxFrictionalForce = 130.0f;
    const ee_float scale = float_from_bits(0x39638E39u);
    const _sFPhysLibColSphere *sphere = car.pSphere.get();
    data.frontWheels.maxDrivingImpulse = data.frontWheels.maxDrivingImpulse * sphere[0].radius * scale;
    data.maxBoatThrust = 450.0f;
    data.waterDragCoef = 300.0f;
    data.waterDragCoefForwards = 25.0f;
    data.rudderConstant = 400.0f;
    data.bouyancyConstant = 100.0f;
    data.rearWheels.maxDrivingImpulse = data.rearWheels.maxDrivingImpulse * sphere[2].radius * scale;
    data.motorOffset = {F(0.0), float_from_bits(0xC5915000u), float_from_bits(0xC69C4000u)};
    return 0;
}

// The test car's nine collision spheres: four wheels, a big body sphere and four corners.
int32_t FPhysLibInitTESTCarSpheres(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    _sFPhysLibColSphere *sphere = car.pSphere.get();
    car.numSpheres = 9;
    const ee_float wheelRadius = float_from_bits(0x455AC000u), wheelY = float_from_bits(0xC5915000u);
    const ee_float frontZ = float_from_bits(0x46192000u), rearZ = float_from_bits(0xC6192000u);
    const ee_float cornerFrontZ = float_from_bits(0x4625A000u), cornerRearZ = float_from_bits(0xC625A000u);
    const ee_float cornerLeft = float_from_bits(0xC573C000u), cornerRight = float_from_bits(0x4573C000u);
    sphere[0].radius = wheelRadius;
    G(bUsingTestSpheres) = 1;
    sphere[0].sphereOffset = {float_from_bits(0xC601B000u), wheelY, frontZ};
    sphere[1].radius = wheelRadius;
    sphere[1].sphereOffset = {float_from_bits(0x4601B000u), wheelY, frontZ};
    sphere[2].radius = wheelRadius;
    sphere[2].sphereOffset = {float_from_bits(0xC604D000u), wheelY, rearZ};
    sphere[3].radius = wheelRadius;
    sphere[3].sphereOffset = {float_from_bits(0x4604D000u), wheelY, rearZ};
    sphere[4].radius = 5600.0f;
    sphere[4].sphereOffset = {F(0.0), F(4000.0), F(0.0)};
    sphere[5].radius = 4000.0f;
    sphere[5].sphereOffset = {cornerLeft, F(2000.0), cornerFrontZ};
    sphere[6].radius = 4000.0f;
    sphere[6].sphereOffset = {cornerRight, F(2000.0), cornerFrontZ};
    sphere[7].radius = 4000.0f;
    sphere[7].sphereOffset = {cornerLeft, F(2000.0), cornerRearZ};
    sphere[8].radius = 4000.0f;
    sphere[8].sphereOffset = {cornerRight, F(2000.0), cornerRearZ};
    return 0;
}

// FPHYSLIB.c

int32_t FPhysLibInit()
{
    FPhysLibGlobals &phys = G(FPhysLib);
    phys.gravity = 500.0f;
    phys.coeffRestitutionContactScaleUp = float_from_bits(0x42CE6667u);
    phys.coeffRestitutionCar = 4000.0f;
    phys.coeffRestitutionGround = 1000.0f;
    phys.powerUpFactor = 4800.0f;
    phys.powerUpFactorSecret = float_from_bits(0x45ABE000u);
    phys.defaultSlidingGrip = float_from_bits(0x453B8000u);
    phys.numCars = 0;
    phys.defaultGrip = 0.0f;
    phys.ticks = 0;
    return 0;
}

int32_t FPhysLibTerm()
{
    G(FPhysLib).numCars = 0;
    return 0;
}

void FCollisionResponse(ptr32<_sFPhysLibCar> pCar);
void FCollisionResponseCarCar(ptr32<_sFPhysLibCar> pCar);
void FCollisionResponseOther(ptr32<_sFPhysLibCar> pCar);
void FCalcCarBodyForces(ptr32<_sFPhysLibCar> pCar);
void FCalcCarWheelForces(ptr32<_sFPhysLibCar> pCar);
void FUpdateCarBody(ptr32<_sFPhysLibCar> pCar);
void FUpdateCarWheels(ptr32<_sFPhysLibCar> pCar);

int32_t FPhysLibCollisionReponse()
{
    FPhysLibGlobals &phys = G(FPhysLib);
    for (int32_t i = 0; i < phys.numCars; ++i)
        if ((phys.pCarList[i]->flags & 2u) == 0u)
            FCollisionResponse(phys.pCarList[i]);
    for (int32_t i = 0; i < phys.numCars; ++i)
    {
        const ptr32<_sFPhysLibCar> car = phys.pCarList[i];
        if ((car->flags & 2u) != 0u)
            continue;
        if ((car->flags & 0x40u) != 0u)
            FCollisionResponseCarCar(car);
        if ((car->flags & 0x80u) != 0u)
            FCollisionResponseOther(car);
    }
    return 0;
}

int32_t FPhysLibAddCar(ptr32<_sFPhysLibCar> pCar)
{
    FPhysLibGlobals &phys = G(FPhysLib);
    phys.pCarList[phys.numCars] = pCar;
    phys.numCars = phys.numCars + 1;
    FInitialiseCar(pCar);
    return phys.numCars;
}

int32_t FPhysLibDeleteCar(ptr32<_sFPhysLibCar> pCar)
{
    FPhysLibGlobals &phys = G(FPhysLib);
    int32_t found = 0;
    while (found < phys.numCars && phys.pCarList[found] != pCar)
        ++found;
    const int32_t last = phys.numCars - 1;
    phys.numCars = last;
    phys.pCarList[found] = phys.pCarList[last];
    phys.pCarList[last] = ptr32<_sFPhysLibCar>(0u);
    return last;
}

int32_t FPhysLibSimulation()
{
    FPhysLibGlobals &phys = G(FPhysLib);
    phys.ticks = phys.ticks + 1;
    for (int32_t i = 0; i < phys.numCars; ++i)
    {
        const ptr32<_sFPhysLibCar> pCar = phys.pCarList[i];
        _sFPhysLibCar &car = *pCar;
        if ((car.flags & 2u) == 0u)
        {
            FCalcCarBodyForces(pCar);
            FCalcCarWheelForces(pCar);
            FUpdateCarBody(pCar);
            FUpdateCarWheels(pCar);
        }
        else
        {
            for (int32_t s = 0; s < car.numSpheres; ++s)
            {
                _sFPhysLibColSphere &sphere = car.pSphere.get()[s];
                sphere.numCarCarCols = 0;
                sphere.numOtherCols = 0;
                sphere.numPolyCols = 0;
                sphere.totalCols = 0;
            }
            car.wheel[3].flags = 0;
            car.wheel[2].flags = 0;
            car.wheel[1].flags = 0;
            car.wheel[0].flags = 0;
            car.flags = car.flags & 0x1Fu;
        }
    }
    return 0;
}

// FRESPONSE.c: turning collisions into impulses.

void FCollisionResponse(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const int32_t numSpheres = car.numSpheres;
    _sFPhysLibColSphere *const spheres = car.pSphere.get();
    sFVec sphereNormal[10];
    ee_float sphereDepth[10];
    int32_t carCarSpheres = 0, otherSpheres = 0;
    ee_float deepestBody = 0.0f; // Among spheres that are not wheels.

    for (int32_t i = 0; i < numSpheres; ++i)
    {
        _sFPhysLibColSphere &sphere = spheres[i];
        sphereDepth[i] = 0.0f;
        if (sphere.numCarCarCols != 0)
            ++carCarSpheres;
        if (sphere.numOtherCols != 0)
            ++otherSpheres;

        ee_float depth = 0.0f;
        sFVec normal;
        normal.x = depth;
        normal.y = depth;
        normal.z = depth;
        int32_t count = 0;
        for (int32_t c = sphere.numCarCarCols + sphere.numOtherCols; c < sphere.totalCols; ++c)
        {
            if (sphere.type[c] == 3)
                continue;
            if (sphere.type[c] == 1 && sphere.polyPtr[c]->gid == 6)
            {
                // A boost strip.
                const ee_float boost = G(boostScale) * kS;
                const sColCellVecPSX &v = gridVector(sphere.polyPtr[c]->sid);
                car.boostImpulse.x = v.x * boost;
                car.boostImpulse.y = v.y * boost;
                car.boostImpulse.z = v.z * boost;
            }
            normal.x = normal.x + sphere.normal[c].x;
            normal.y = normal.y + sphere.normal[c].y;
            normal.z = normal.z + sphere.normal[c].z;
            ++count;
            if (!(sphere.normal[c].depth <= depth))
                depth = sphere.normal[c].depth;
        }

        if (F(0.0) < depth && (normal.x != F(0.0) || normal.y != F(0.0) || normal.z != F(0.0)))
        {
            sphereDepth[i] = depth;
            if (i > 3 && deepestBody < depth)
                deepestBody = depth;
            if (count < 2)
            {
                sphereNormal[i] = normal;
            }
            else
            {
                const ee_float unit = ee_rsqrt(4096.0f, normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                sphereNormal[i].x = normal.x * unit;
                sphereNormal[i].y = normal.y * unit;
                sphereNormal[i].z = normal.z * unit;
            }
            sphere.avNormal = sphereNormal[i];
        }
    }

    const ee_float upX = car.mat.Xy, upY = car.mat.Yy, upZ = car.mat.Zy;
    const _sFPhysLibCarData &data = *car.pData;
    int32_t bodyContacts = 0;

    for (int32_t i = 0; i < numSpheres; ++i)
    {
        _sFPhysLibColSphere &sphere = spheres[i];
        int32_t contact = sphere.contactCounter;
        if (sphereDepth[i] == F(0.0))
        {
            if (contact < 1)
            {
                if (contact > -30)
                    --contact;
            }
            else
            {
                contact = 0;
            }
            sphere.contactCounter = static_cast<int16>(contact);
            continue;
        }

        if (contact < 1)
            contact = 1;
        else if (contact < 30)
            ++contact;
        sphere.contactCounter = static_cast<int16>(contact);

        if (i <= 3)
            continue;
        ++bodyContacts;
        if (deepestBody == F(0.0) || sphereDepth[i] != deepestBody)
            continue;

        // Only the deepest body sphere pushes the car out and takes an impulse.
        const sFVec &n = sphereNormal[i];
        const ee_float push = sphereDepth[i] * kS;
        const ee_float pushX = n.x * push, pushY = n.y * push, pushZ = n.z * push;
        car.pos.x = car.pos.x + pushX;
        car.pos.y = car.pos.y + pushY;
        car.pos.z = car.pos.z + pushZ;
        if (tooLarge(pushX) || tooLarge(pushY) || tooLarge(pushZ))
            car.flags = car.flags | 1u;

        ee_float closing = -((car.vel.x * n.x + car.vel.y * n.y + car.vel.z * n.z) * kS);
        if (closing < F(0.0))
            closing = 0.0f;
        const ee_float slide = closing * kS;
        sFVec friction;
        friction.x = car.vel.x + n.x * slide;
        friction.y = car.vel.y + n.y * slide;
        friction.z = car.vel.z + n.z * slide;
        const int32_t lastCol = sphere.totalCols - 1;
        if (sphere.type[lastCol] == 1 && sphere.polyPtr[lastCol]->gid == 3)
        {
            // A moving surface: friction acts on the speed relative to it.
            const sColCellVecPSX &v = gridVector(sphere.polyPtr[lastCol]->sid);
            friction.x = friction.x - v.x;
            friction.y = friction.y - v.y;
            friction.z = friction.z - v.z;
        }

        ee_float frictionScale;
        if (car.mode == 2 && float_from_bits(0x45354000u) < ee_abs(n.y))
            frictionScale = -data.colFricConstant * F(0.5);
        else
            frictionScale = -data.colFricConstant;
        frictionScale = frictionScale * kS;

        ee_float restitution = G(FPhysLib).coeffRestitutionGround + ee_itof(contact) * G(FPhysLib).coeffRestitutionContactScaleUp;
        if (F(8000.0) < closing)
            restitution = restitution + ((closing - F(8000.0)) * F(4096.0)) / closing;

        const ee_float bounce = closing * restitution * kS * data.mass * kS * kS;
        sFVec impulse;
        impulse.x = n.x * bounce + friction.x * frictionScale;
        impulse.y = n.y * bounce + friction.y * frictionScale;
        impulse.z = n.z * bounce + friction.z * frictionScale;
        addImpulse(car, impulse);

        if (i != 4)
        {
            impulse.x = impulse.x * F(0.5);
            impulse.y = impulse.y * F(0.5);
            impulse.z = impulse.z * F(0.5);
            addTorque(car, impulse, surfaceOffset(car, sphere, n));
        }
    }

    uint8_t flags = car.flags;
    flags = bodyContacts == 0 ? flags & 0xDFu : flags | 0x20u;
    flags = carCarSpheres == 0 ? flags & 0xBFu : flags | 0x40u;
    flags = otherSpheres == 0 ? flags & 0x7Fu : flags | 0x80u;
    car.flags = flags;

    if (car.mode == 1)
    {
        const ee_float threshold = G(cosineThreshold);
        ee_float limit = data.frontWheels.suspLength;
        for (int i = 0; i < 4; ++i)
        {
            if (i == 2)
                limit = data.rearWheels.suspLength;
            _sFPhysLibWheel &wheel = car.wheel[i];
            const ee_float depth = sphereDepth[i];
            if (depth == F(0.0))
            {
                wheel.flags = wheel.flags & 0xFFFEu;
                continue;
            }
            const ee_float cosine = (upX * sphereNormal[i].x + upY * sphereNormal[i].y + upZ * sphereNormal[i].z) * kS;
            wheel.flags = wheel.flags | 1u;
            if (!(threshold <= cosine))
                continue;

            // The wheel is pushed up its suspension, within the travel available.
            const ee_float vertical = wheel.vertPos;
            ee_float move = (depth * F(4096.0)) / cosine;
            if (limit < vertical + move)
                move = limit - vertical;
            else if (vertical + move < -limit)
                move = -limit - vertical;

            sFVec suspended;
            suspended.x = wheel.suspOffset.x;
            suspended.y = wheel.suspOffset.y + (vertical + move);
            suspended.z = wheel.suspOffset.z;
            wheel.vertVel = wheel.vertVel + move;
            wheel.vertPos = vertical + move;
            const sFVec world = matMul(car.mat, suspended);
            wheel.localPos = world;
            wheel.pos.x = car.pos.x + world.x;
            wheel.pos.y = car.pos.y + world.y;
            wheel.pos.z = car.pos.z + world.z;
        }
    }
    else
    {
        car.wheel[3].flags = 0;
        car.wheel[0].flags = 0;
        car.wheel[1].flags = 0;
        car.wheel[2].flags = 0;
    }

    updateRenderPos(car, data, car.pos);
}

// Collisions with things other than cars and the track: pushes the car away from the deepest one.
void FCollisionResponseOther(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibColSphere *const spheres = car.pSphere.get();
    // With no contact at depth 0 or more, normal is the stale stack local (sp+0 of the original's 0x40 frame).
    sFVec normal;
    normal.x = stale_stack<ee_float>(0x40, 0);
    normal.y = stale_stack<ee_float>(0x40, 4);
    normal.z = stale_stack<ee_float>(0x40, 8);
    int32_t bestSphere = -1;
    ee_float best = -1.0f;
    const ee_float impulseX = car.impulse.x;

    for (int32_t i = 0; i < car.numSpheres; ++i)
    {
        const _sFPhysLibColSphere &sphere = spheres[i];
        int32_t index = -1;
        ee_float depth = 0.0f;
        for (int32_t c = sphere.numCarCarCols + sphere.numOtherCols - 1; c >= sphere.numCarCarCols; --c)
        {
            if (!(sphere.normal[c].depth < depth))
            {
                index = c;
                depth = sphere.normal[c].depth;
            }
        }
        if (index >= 0 && best < depth)
        {
            normal.x = sphere.normal[index].x;
            normal.y = sphere.normal[index].y;
            normal.z = sphere.normal[index].z;
            bestSphere = i;
            best = depth;
        }
    }

    ee_float magnitude = best * G(rpMulMag);
    if (G(rpMaxMag) < magnitude)
        magnitude = G(rpMaxMag);
    magnitude = magnitude * kS;

    sFVec impulse;
    impulse.x = normal.x * magnitude;
    impulse.y = normal.y * magnitude;
    impulse.z = normal.z * magnitude;
    car.impulse.x = impulseX + impulse.x;
    car.impulse.y = car.impulse.y + impulse.y;
    car.impulse.z = car.impulse.z + impulse.z;
    addScaledTorque(car, impulse, surfaceOffset(car, spheres[bestSphere], normal), F(0.5));
}

void FCollisionResponseCarCar(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibColSphere *const spheres = car.pSphere.get();
    const _sFPhysLibCarData &data = *car.pData;
    // With no car contact at depth 0 or more (bestSphere stays -1), the original never sets these: normal is the stale
    // stack local at sp+0 of its 0x50 frame and the other car is whatever its caller left in $t6.
    sFVec normal;
    normal.x = stale_stack<ee_float>(0x50, 0);
    normal.y = stale_stack<ee_float>(0x50, 4);
    normal.z = stale_stack<ee_float>(0x50, 8);
    ptr32<_sFPhysLibCar> other(getRegU32(&g_runtime->eeScheduler().runningContext(), 14));
    int32_t bestSphere = -1;
    ee_float best = -1.0f;
    const sFVec startPos = car.pos;
    const ee_float impulseX = car.impulse.x;

    for (int32_t i = 0; i < car.numSpheres; ++i)
    {
        const _sFPhysLibColSphere &sphere = spheres[i];
        int32_t index = -1;
        ee_float depth = 0.0f;
        for (int32_t c = 0; c < sphere.numCarCarCols; ++c)
        {
            if (!(sphere.normal[c].depth < depth))
            {
                depth = sphere.normal[c].depth;
                index = c;
            }
        }
        if (index >= 0 && best < depth)
        {
            normal.x = sphere.normal[index].x;
            normal.y = sphere.normal[index].y;
            normal.z = sphere.normal[index].z;
            other = sphere.carPtr[index]; // Shares its slot with polyPtr.
            bestSphere = i;
            best = depth;
        }
    }

    const ee_float push = best * kS;
    car.pos.y = startPos.y + normal.y * push * F(0.25);
    car.pos.x = startPos.x + normal.x * push * F(0.25);
    car.pos.z = startPos.z + normal.z * push * F(0.25);
    if (tooLarge(normal.x * push) || tooLarge(normal.y * push) || tooLarge(normal.z * push))
        car.flags = car.flags | 1u;

    ee_float closing = -(((car.vel.x - other->vel.x) * normal.x + (car.vel.y - other->vel.y) * normal.y + (car.vel.z - other->vel.z) * normal.z) * kS);
    if (closing < F(0.0))
        closing = 0.0f;
    const ee_float bounce = closing * G(FPhysLib).coeffRestitutionCar * kS * data.mass * kS * kS;

    sFVec impulse;
    impulse.x = normal.x * bounce;
    impulse.z = normal.z * bounce;
    impulse.y = normal.y * bounce;
    car.impulse.x = impulseX + impulse.x;
    car.impulse.y = car.impulse.y + impulse.y;
    car.impulse.z = car.impulse.z + impulse.z;

    if (bestSphere != 4)
        addScaledTorque(car, impulse, surfaceOffset(car, spheres[bestSphere], normal), F(0.25));
}

// FUPDATE.c: integrating impulses into motion.

void FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar> pCar, int32_t bFullUpdate)
{
    _sFPhysLibCar &car = *pCar;
    const uint8_t startFlags = car.flags;
    _sFPhysLibWheel *wheel = car.wheel;
    int32_t count = car.numSpheres;
    _sFPhysLibColSphere *sphere = car.pSphere.get();
    const sFVec carPos = car.pos;

    if (bFullUpdate == 0)
        count = 4;
    else if ((startFlags & 1u) != 0u)
        car.flags = startFlags & 0xFEu;

    for (int32_t i = 0; i < count; ++i, ++sphere)
    {
        sFVec world;
        if (i < 4 && car.mode == 1)
        {
            sFVec suspended;
            suspended.x = wheel->suspOffset.x;
            suspended.y = wheel->suspOffset.y + wheel->vertPos;
            suspended.z = wheel->suspOffset.z;
            const sFVec local = matMul(car.mat, suspended);
            world.z = carPos.z + local.z;
            world.x = carPos.x + local.x;
            world.y = carPos.y + local.y;
            wheel->pos = world;
            wheel->localPos = local;
            ++wheel;
        }
        else
        {
            const sFVec &offset = sphere->sphereOffset;
            if (offset.x == F(0.0) && offset.y == F(0.0) && offset.z == F(0.0))
            {
                world = carPos;
            }
            else
            {
                world.y = carPos.y + rowMul(car.mat.Yx, car.mat.Yy, car.mat.Yz, offset.x, offset.y, offset.z);
                world.x = carPos.x + rowMul(car.mat.Xx, car.mat.Xy, car.mat.Xz, offset.x, offset.y, offset.z);
                world.z = carPos.z + rowMul(car.mat.Zx, car.mat.Zy, car.mat.Zz, offset.x, offset.y, offset.z);
            }
        }

        if (bFullUpdate == 0)
            continue;

        sphere->numCarCarCols = 0;
        sphere->numOtherCols = 0;
        const ee_float radius = sphere->radius * F(0.00390625);
        sphere->numPolyCols = 0;
        sphere->totalCols = 0;
        const ee_float x16 = world.x * F(0.00390625);
        const ee_float y16 = world.y * F(0.00390625);
        sphere->colFlags = 0;
        const ee_float z16 = world.z * F(0.00390625);
        if ((startFlags & 1u) == 0u)
        {
            sphere->oldPos16.x = sphere->pos16.x;
            sphere->oldPos16.y = sphere->pos16.y;
            sphere->oldPos16.z = sphere->pos16.z;
        }
        else
        {
            sphere->oldPos16.x = x16;
            sphere->oldPos16.y = y16;
            sphere->oldPos16.z = z16;
        }
        if (i < 4 && car.mode == 1 && sphere->contactCounter < -0x14)
            sphere->oldPos16.y = sphere->oldPos16.y + radius * F(3.0);

        const ee_float oldX = sphere->oldPos16.x, oldY = sphere->oldPos16.y, oldZ = sphere->oldPos16.z;
        sphere->pos = world;
        sphere->pos16.x = x16;
        sphere->pos16.y = y16;
        sphere->pos16.z = z16;
        const bool xNewLower = x16 <= oldX;
        sphere->min.x = (xNewLower ? x16 : oldX) - radius;
        sphere->max.x = (xNewLower ? oldX : x16) + radius;
        const bool yNewLower = y16 <= oldY;
        sphere->min.y = (yNewLower ? y16 : oldY) - radius;
        sphere->max.y = (yNewLower ? oldY : y16) + radius;
        const bool zNewLower = z16 <= oldZ;
        sphere->max.z = (zNewLower ? oldZ : z16) + radius;
        sphere->min.z = (zNewLower ? z16 : oldZ) - radius;
    }

    if (bFullUpdate != 0)
        updateRenderPos(car, *car.pData, carPos);
}

void FUpdateCarWheels(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibCarData &data = *car.pData;
    if (car.mode == 1)
    {
        for (int i = 0; i < 4; ++i)
        {
            const _sFPhysLibWheelData &params = i < 2 ? data.frontWheels : data.rearWheels;
            _sFPhysLibWheel &wheel = car.wheel[i];
            const ee_float limit = params.suspLength;
            // In the original's order: the scale by 1/4096 before the mass. A power of two scales exactly, except when
            // the spring has decayed into the denormals (a wheel long at rest), where the order changes the result.
            const ee_float spring = -(wheel.vertPos * params.springConstant + wheel.vertVel * params.springDampConstant) * kS;
            wheel.vertVel = wheel.vertVel + params.invMass * spring * kS;
            wheel.vertPos = wheel.vertPos + wheel.vertVel * kTimestep;
            if (limit < wheel.vertPos)
                wheel.vertPos = limit;
            else if (wheel.vertPos < -limit)
                wheel.vertPos = -limit;
        }
    }

    FPhysLibUpdateCarSpheres(pCar, 1);

    if (car.mode != 1)
        return;
    const ee_float spinToAngle = float_from_bits(0x4022F987u);
    for (int i = 0; i < 4; ++i)
    {
        const ee_float inverseInertia = (i < 2 ? data.frontWheels : data.rearWheels).invInertia;
        _sFPhysLibWheel &wheel = car.wheel[i];
        const uint16_t flags = wheel.flags;
        wheel.angVel = wheel.angVel + wheel.angImpulse * inverseInertia * kS;
        if (car.inputBrake == 0xFF && i > 1)
        {
            // Handbrake: rear wheels lock.
            wheel.angVel = 0.0f;
            wheel.flags = flags | 4u;
        }
        else if ((flags & 4u) != 0u)
        {
            wheel.flags = flags & 0xFFFBu;
            wheel.angVel = car.wheel[i == 2 ? 0 : 1].angVel;
        }
        wheel.angImpulse = 0.0f;
        wheel.angPos = ee_itof(ee_ftoi(wheel.angPos + wheel.angVel * spinToAngle) & 0xFFFF);
    }
}

void FUpdateOrientationFromAngVel(ptr32<_sFMat> pMat, ptr32<_sFVec> pAngVel)
{
    updateOrientation(*pMat, *pAngVel);
}

void FUpdateCarBody(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const uint8_t startFlags = car.flags;
    const _sFPhysLibCarData &data = *car.pData;

    car.impulse.x = car.impulse.x + car.boostImpulse.x;
    car.impulse.y = car.impulse.y + car.boostImpulse.y;
    car.impulse.z = car.impulse.z + car.boostImpulse.z;
    car.boostImpulse.z = 0.0f;
    car.boostImpulse.y = 0.0f;
    car.boostImpulse.x = 0.0f;

    const ee_float inverseMass = data.invMass * kS;
    const ee_float accX = car.impulse.x * inverseMass;
    const ee_float accY = car.impulse.y * inverseMass;
    const ee_float accZ = car.impulse.z * inverseMass;
    car.vel.x = car.vel.x + accX * kTimestep;
    car.vel.y = car.vel.y + accY * kTimestep;
    car.vel.z = car.vel.z + accZ * kTimestep;
    car.acc.x = accX;
    car.acc.y = accY;
    car.acc.z = accZ;

    const ee_float vx = car.vel.x, vy = car.vel.y, vz = car.vel.z;
    const ee_float kMaxSpeed = float_from_bits(0x46CF0800u); // 26500
    ee_float speed = ee_sqrt(vx * vx + vy * vy + vz * vz);
    if (kMaxSpeed < speed)
    {
        const ee_float scale = (float_from_bits(0x4CCF0800u) / speed) * kS;
        car.vel.x = vx * scale;
        car.vel.y = car.vel.y * scale;
        car.vel.z = car.vel.z * scale;
        speed = kMaxSpeed;
    }
    car.speed = speed;

    if (car.mode == 1)
    {
        if (speed < F(16.0))
        {
            car.vel.x = 0.0f;
            car.vel.y = 0.0f;
            car.vel.z = 0.0f;
            car.speed = 0.0f;
        }
        else if (speed < F(32.0))
        {
            // Fades out to a dead stop.
            const ee_float fade = (speed - F(16.0)) * F(256.0);
            const ee_float scale = fade * kS;
            car.vel.x = car.vel.x * scale;
            car.vel.y = car.vel.y * scale;
            car.vel.z = car.vel.z * scale;
            car.speed = speed * fade * kS;
        }
    }

    if ((startFlags & 4u) != 0u)
    {
        car.vel.x = 0.0f;
        car.vel.z = 0.0f;
    }
    car.pos.x = car.pos.x + car.vel.x * kTimestep;
    car.pos.y = car.pos.y + car.vel.y * kTimestep;
    car.pos.z = car.pos.z + car.vel.z * kTimestep;

    const ee_float inverseInertia = data.invInertiaX * F(8.0) * kS;
    car.angVel.x = car.angVel.x + car.angImpulse.x * inverseInertia * kTimestep;
    car.angVel.y = car.angVel.y + car.angImpulse.y * inverseInertia * kTimestep;
    car.angVel.z = car.angVel.z + car.angImpulse.z * inverseInertia * kTimestep;
    if ((startFlags & 8u) != 0u)
    {
        car.angVel.x = 0.0f;
        car.angVel.z = 0.0f;
        car.angVel.y = 0.0f;
    }

    ee_float spinX = car.angVel.x;
    if (car.mode == 1)
    {
        if (ee_abs(spinX) < F(16.0))
        {
            car.angVel.x = 0.0f;
            spinX = car.angVel.x;
        }
        if (ee_abs(car.angVel.z) < F(16.0))
            car.angVel.z = 0.0f;
    }
    const ee_float spinZ = car.angVel.z;

    // A spin below the limit is replaced by the positive limit, as the original does.
    const ee_float kMaxSpin = F(10000.0), kMinSpin = F(-10000.0);
    ee_float clampedX = kMaxSpin;
    if (kMaxSpin < spinX)
        car.angVel.x = kMaxSpin;
    else if (kMinSpin <= spinX)
        clampedX = spinX;
    else
        car.angVel.x = kMaxSpin;

    const ee_float spinY = car.angVel.y;
    if (kMaxSpin < spinY || spinY < kMinSpin)
        car.angVel.y = kMaxSpin;
    if (kMaxSpin < spinZ || spinZ < kMinSpin)
        car.angVel.z = kMaxSpin;

    sFVec turn;
    turn.x = clampedX * kTimestep;
    turn.y = car.angVel.y * kTimestep;
    turn.z = car.angVel.z * kTimestep;
    updateOrientation(car.mat, turn);

    const uint32_t tick = static_cast<uint16_t>(car.tickCount);
    car.tickCount = static_cast<int16>(tick + 1u);
    if ((tick & 0xFFu) == 0u)
    {
        const ptr32<_sFMat> mat(guest_addr(&car.mat));
        FMatNormalise(mat, mat);
    }

    car.impulse.x = 0.0f;
    car.impulse.y = 0.0f;
    car.impulse.z = 0.0f;
    car.angImpulse.x = 0.0f;
    car.angImpulse.y = 0.0f;
    car.angImpulse.z = 0.0f;

    _sFMat &front = car.frontWheelMat;
    front.Xx = 4096.0f;
    front.Zx = 0.0f;
    front.Zz = 4096.0f;
    front.Yy = 4096.0f;
    front.Xz = 0.0f;
    front.Xy = 0.0f;
    front.Yz = 0.0f;
    front.Yx = 0.0f;
    front.Zy = 0.0f;

    const ee_float steer = car.mode == 2 ? ee_itof(-static_cast<int32_t>(car.inputSteerAngle) * data.maxSteerAngleBoat) * F(0.25)
                                         : ee_itof(static_cast<int32_t>(car.inputSteerAngle) * data.maxSteerAngle);
    const ee_float kSteerScale = float_from_bits(0x38C90FDBu);
    const ee_float c = ee_cosf(steer * kSteerScale);
    const ee_float s = ee_sinf(steer * kSteerScale);

    const ee_float oldXx = front.Xx;
    front.Xx = c * oldXx + s * front.Zx;
    const ee_float oldXy = front.Xy, oldZy = front.Zy, oldZz = front.Zz, oldXz = front.Xz;
    front.Zx = c * front.Zx - s * oldXx;
    front.Xy = c * oldXy + s * oldZy;
    front.Zy = c * oldZy - s * oldXy;
    front.Xz = c * oldXz + s * oldZz;
    front.Zz = c * oldZz - s * oldXz;
    fmatMul(front, car.mat, front);
}

// F3DMATH.c

void FMatRotateY(ptr32<_sFMat> pMatrix, ee_float psxangle)
{
    _sFMat &m = *pMatrix;
    const ee_float angle = psxangle * float_from_bits(0x38C90FDBu);
    const ee_float c = ee_cosf(angle), s = ee_sinf(angle);
    const ee_float oldXx = m.Xx, oldXy = m.Xy, oldXz = m.Xz;
    m.Xx = c * oldXx + s * m.Zx;
    m.Zx = c * m.Zx - s * oldXx;
    m.Xz = c * oldXz + s * m.Zz;
    m.Xy = c * oldXy + s * m.Zy;
    m.Zy = c * m.Zy - s * oldXy;
    m.Zz = c * m.Zz - s * oldXz;
}

// FFORCES.c

void FCalcCarWheelForces(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    if (car.mode != 1)
        return;
    const _sFPhysLibCarData &data = *car.pData;
    const FPhysLibGlobals &phys = G(FPhysLib);

    const ee_float powerUp = ee_itof(car.powerUpCounter);
    ee_float frontDrive = data.frontWheels.maxDrivingImpulse;
    ee_float frontDamp = data.frontWheels.axleDampingConstant;
    if (powerUp != F(0.0))
    {
        car.powerUpCounter = static_cast<int16>(ee_ftoi(powerUp - F(1.0)));
        const ee_float factor = car.inputThrottleHard != 0 ? phys.powerUpFactorSecret : phys.powerUpFactor;
        frontDrive = frontDrive * factor * kS;
        if (car.inputBrake == 1)
            frontDamp = frontDamp * F(4.0);
    }

    ee_float rearDrive = data.rearWheels.maxDrivingImpulse;
    ee_float rearDamp = data.rearWheels.axleDampingConstant;
    if (car.inputBrake == 1)
        rearDamp = rearDamp * F(4.0);
    if (powerUp != F(0.0))
    {
        const ee_float factor = car.inputThrottleHard == 0 ? phys.powerUpFactor : phys.powerUpFactorSecret;
        rearDrive = rearDrive * factor * kS;
    }

    const int16_t throttle = car.inputEngineThrottle;
    for (int i = 0; i < 4; ++i)
    {
        _sFPhysLibWheel &wheel = car.wheel[i];
        const ee_float drive = i < 2 ? frontDrive : rearDrive;
        const ee_float damp = i < 2 ? frontDamp : rearDamp;
        wheel.angImpulse = (wheel.angImpulse + ee_itof(throttle) * drive * F(0.0078125)) - wheel.angVel * damp * kS;
    }

    const ee_float upX = car.mat.Xy, upY = car.mat.Yy, upZ = car.mat.Zy;
    sFVec forwards;
    forwards.x = car.frontWheelMat.Xz;
    forwards.y = car.frontWheelMat.Yz;
    forwards.z = car.frontWheelMat.Zz;

    ee_float radius = data.frontWheels.radius;
    ee_float grip = data.frontWheels.gripConstant;
    ee_float maxNormal = data.frontWheels.maxNormalForce;
    ee_float maxFriction = data.frontWheels.maxFrictionalForce;
    const ee_float maxFrictionSquared = data.frontWheels.maxFrictionalForce2; // The original uses the front value for all four.
    ee_float wheelSpeedSum = 0.0f;
    const _sFPhysLibColSphere *sphere = car.pSphere.get();

    for (int i = 0; i < 4; ++i, ++sphere)
    {
        _sFPhysLibWheel &wheel = car.wheel[i];
        int32_t oily = wheel.oilyCounter;
        const uint16_t flags = wheel.flags;
        if (i == 2)
        {
            forwards.x = car.mat.Xz;
            forwards.y = car.mat.Yz;
            forwards.z = car.mat.Zz;
            radius = data.rearWheels.radius;
            grip = data.rearWheels.gripConstant;
            maxNormal = data.rearWheels.maxNormalForce;
            maxFriction = data.rearWheels.maxFrictionalForce;
        }
        if (oily != 0)
            wheel.oilyCounter = static_cast<int16>(oily - 1);
        wheelSpeedSum = wheelSpeedSum + wheel.angVel * radius * kS;

        if ((flags & 1u) == 0u || !(F(80.0) <= wheel.vertImpulse))
            continue;

        ee_float normalForce = wheel.vertImpulse;
        if (maxNormal < normalForce)
            normalForce = maxNormal;
        if (i > 1 && car.inputBrake == 0xFF)
            oily = 1;

        const ee_float angX = car.angVel.x, angY = car.angVel.y;
        const ee_float angZ8 = car.angVel.z * F(0.125);
        const ee_float pointX = (wheel.localPos.y * angZ8 - wheel.localPos.z * angY * F(0.125)) * kS + car.vel.x;
        const ee_float pointY = (wheel.localPos.z * angX * F(0.125) - wheel.localPos.x * angZ8) * kS + car.vel.y;
        const ee_float rolling = radius * wheel.angVel * kS * kS;
        const ee_float pointZ = (wheel.localPos.x * angY * F(0.125) - wheel.localPos.y * angX * F(0.125)) * kS + car.vel.z;
        const ee_float alongUp = (pointX * upX + pointY * upY + pointZ * upZ) * kS * kS;

        sFVec slip;
        slip.x = (pointX - upX * alongUp) - forwards.x * rolling;
        slip.y = (pointY - upY * alongUp) - forwards.y * rolling;
        slip.z = (pointZ - upZ * alongUp) - forwards.z * rolling;

        uint32_t material = 0x1Du;
        if (sphere->type[sphere->totalCols - 1] == 1)
        {
            const _sColPolyPSX &poly = *sphere->polyPtr[sphere->totalCols - 1];
            if (poly.gid == 3)
            {
                const sColCellVecPSX &v = gridVector(poly.sid);
                slip.x = slip.x - v.x;
                slip.y = slip.y - v.y;
                slip.z = slip.z - v.z;
            }
            material = poly.mat;
        }

        const ee_float damping = ee_itof(G(mtlG).mtl[material].damping);
        const ee_float gripScale = -(grip + ee_itof(G(mtlG).mtl[material].grip)) * normalForce * kS * kS;
        sFVec friction;
        friction.x = slip.x * gripScale;
        friction.y = slip.y * gripScale;
        friction.z = slip.z * gripScale;

        const ee_float squared = friction.x * friction.x + friction.y * friction.y + friction.z * friction.z;
        if (maxFrictionSquared < squared)
        {
            // Past the limit of adhesion: the wheel is sliding.
            const ee_float scale = ee_rsqrt(maxFriction * F(4096.0), squared) * kS;
            friction.z = friction.z * scale;
            friction.x = friction.x * scale;
            friction.y = friction.y * scale;
            if ((flags & 2u) == 0u)
                wheel.flags = flags | 2u;
        }
        else if ((flags & 2u) != 0u)
        {
            wheel.flags = flags & 0xFFFDu;
        }

        if (oily != 0)
        {
            friction.x = friction.x * F(0.5);
            friction.y = friction.y * F(0.5);
            friction.z = friction.z * F(0.5);
        }
        if (damping != F(0.0))
        {
            const ee_float drag = -damping * F(0.25) * kS;
            friction.z = friction.z + pointZ * drag;
            friction.x = friction.x + pointX * drag;
            friction.y = friction.y + pointY * drag;
        }

        wheel.angImpulse = wheel.angImpulse + -radius * (forwards.x * friction.x + forwards.y * friction.y + forwards.z * friction.z) * kS * kS;
        addImpulse(car, friction);
        addTorque(car, friction, wheel.localPos);
    }

    ee_float rpm = ee_abs(wheelSpeedSum * F(0.25) * F(0.00390625));
    if (F(255.0) < rpm)
        rpm = 255.0f;
    car.engineRpm = ee_ftoi(rpm);
}

void FCalcCarBodyForces(ptr32<_sFPhysLibCar> pCar);

// FBOAT.c

void FCalcBoatMotorForces(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibCarData &data = *car.pData;

    ee_float thrust = data.maxBoatThrust;
    if (ee_itof(car.powerUpCounter) != F(0.0))
    {
        thrust = thrust * G(FPhysLib).powerUpFactor * kS;
        car.powerUpCounter = static_cast<int16>(ee_ftoi(ee_itof(car.powerUpCounter) - F(1.0)));
    }

    const int32_t throttle = car.inputEngineThrottle;
    const int32_t absThrottle = throttle < 0 ? -throttle : throttle;
    const _sFPhysLibColSphere *const spheres = car.pSphere.get();
    if ((spheres[2].liquidDepth + spheres[3].liquidDepth) * F(0.5) <= F(0.0))
    {
        car.engineRpm = absThrottle; // The motor is out of the water.
        return;
    }

    thrust = ee_itof(throttle) * thrust * F(0.0078125);
    if (thrust < F(0.0))
        thrust = thrust * F(0.25);
    thrust = thrust * kS;

    const ee_float rudder = ee_itof(-static_cast<int32_t>(car.inputSteerAngle)) *
                            (car.mat.Xz * car.vel.x + car.mat.Yz * car.vel.y + car.mat.Zz * car.vel.z) * kS * kS * data.rudderConstant * kS *
                            (F(128.0) - ee_abs(ee_itof(throttle))) * F(0.0078125) * kS;
    ee_float upright = car.mat.Yy;
    if (upright < F(0.0))
        upright = 0.0f;
    upright = upright * kS;

    sFVec force;
    force.x = (car.frontWheelMat.Xz * thrust + car.mat.Xx * rudder) * upright;
    force.z = (car.frontWheelMat.Zz * thrust + car.mat.Zx * rudder) * upright;
    force.y = (car.frontWheelMat.Yz * thrust + car.mat.Yx * rudder) * upright;

    const sFVec offset = matMul(car.mat, data.motorOffset);
    addImpulse(car, force);
    addTorque(car, force, offset);
    car.engineRpm = ee_div(absThrottle, 2);
}

void FCalcWaterForces(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibCarData &data = *car.pData;
    _sFPhysLibColSphere *sphere = car.pSphere.get();

    ee_float carFwdsX = car.mat.Xz, carFwdsZ = car.mat.Zz;
    const ee_float forwardDrag = data.waterDragCoefForwards;
    const ee_float buoyancy = data.bouyancyConstant;
    const ee_float sideDrag = data.waterDragCoef;
    if (carFwdsX != F(0.0) || carFwdsZ != F(0.0))
    {
        const ee_float unit = ee_rsqrt(4096.0f, carFwdsX * carFwdsX + carFwdsZ * carFwdsZ);
        carFwdsZ = carFwdsZ * unit;
        carFwdsX = carFwdsX * unit;
    }

    // A small bob up and down, from a table of 32 sine values in the game's data.
    const int32_t ticks = G(FPhysLib).ticks;
    const ee_float *const table = static_cast<const ee_float *>(guest_host((sym::sec_rodata + 0x24f8u)));
    const ee_float bob = table[(static_cast<uint32_t>(ticks - (ticks >> 31)) & 0x3Eu) / 2u] * G(perturbScaleG);

    for (int i = 0; i < 5; ++i, ++sphere)
    {
        const ee_float flowScale = G(waterFlowScale);
        ee_float depth = sphere->liquidDepth;
        if (!(F(0.0) < depth))
            continue;

        const ee_float diameter = sphere->radius + sphere->radius;
        if (diameter < depth)
            depth = diameter;
        depth = depth + bob * kS;

        const ee_float smoothX = ((sphere->smoothedWaterFlow.x * F(16.0) - sphere->smoothedWaterFlow.x) + sphere->waterFlow.x) * F(0.0625);
        const ee_float flowX = smoothX * flowScale;
        sphere->smoothedWaterFlow.x = smoothX;
        sphere->smoothedWaterFlow.y = ((sphere->smoothedWaterFlow.y * F(16.0) - sphere->smoothedWaterFlow.y) + sphere->waterFlow.y) * F(0.0625);
        sphere->smoothedWaterFlow.z = ((sphere->smoothedWaterFlow.z * F(16.0) - sphere->smoothedWaterFlow.z) + sphere->waterFlow.z) * F(0.0625);

        const ee_float relX = car.vel.x - flowX * kS;
        const ee_float dragScale = -(sideDrag * depth * kS) * kS;
        const ee_float relY = car.vel.y - sphere->smoothedWaterFlow.y * flowScale * kS;
        const ee_float relZ = car.vel.z - sphere->smoothedWaterFlow.z * flowScale * kS;
        const ee_float antiDrag = -((carFwdsX * relX + relY * F(0.0) + carFwdsZ * relZ) * kS * (sideDrag - forwardDrag) * kS * depth * kS) * kS;

        const ee_float size = sphere->radius * kS;
        ee_float dragY = (relY * dragScale - antiDrag * F(0.0)) * size;
        sFVec force;
        force.x = (relX * dragScale - carFwdsX * antiDrag) * size;
        force.z = (relZ * dragScale - carFwdsZ * antiDrag) * size;
        if (dragY < F(0.0))
            dragY = dragY * F(0.25);

        ee_float lift = sphere->radius;
        if (depth < lift)
            lift = lift - depth;
        lift = lift * F(0.015625) * car.speed * F(3.0517578e-05);
        if (i > 1)
            lift = lift * F(0.5);
        if (i > 3)
            lift = lift * F(0.5);
        force.y = dragY + depth * buoyancy * kS + lift;

        addImpulse(car, force);
        addTorque(car, force, matMul(car.mat, sphere->sphereOffset));
    }
}

void FCalcCarBodyForces(ptr32<_sFPhysLibCar> pCar)
{
    _sFPhysLibCar &car = *pCar;
    const _sFPhysLibCarData &data = *car.pData;
    ee_float spinDamp;

    if (car.mode == 1)
    {
        const ee_float upX = car.mat.Xy, upY = car.mat.Yy, upZ = car.mat.Zy;
        ee_float gripSum = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            _sFPhysLibWheel &wheel = car.wheel[i];
            int32_t material = wheel.drivingOnMtl;
            const _sFPhysLibWheelData &params = i < 2 ? data.frontWheels : data.rearWheels;
            if (material == 0x46)
                material = 0;
            const ee_float suspension = (wheel.vertPos * params.springConstant2 + wheel.vertVel * params.springDampConstant) * kS;
            gripSum = gripSum + ee_itof(G(mtlG).mtl[material].grip);
            wheel.vertImpulse = suspension;
            const ee_float scaled = suspension * kS;
            sFVec impulse;
            if ((wheel.flags & 1u) == 0u)
            {
                impulse.x = upX * scaled;
                impulse.z = upZ * scaled;
                impulse.y = upY * scaled;
            }
            else
            {
                const sFVec &normal = car.pSphere.get()[i].avNormal;
                impulse.x = normal.x * scaled;
                impulse.y = normal.y * scaled;
                impulse.z = normal.z * scaled;
            }
            addImpulse(car, impulse);
            addTorque(car, impulse, wheel.localPos);
        }
        spinDamp = -data.angDampConstant * gripSum * F(0.17089844) * F(3.0517578e-05);
    }
    else
    {
        FCalcBoatMotorForces(pCar);
        FCalcWaterForces(pCar);
        spinDamp = -data.angDampConstant * F(0.125);
    }

    spinDamp = spinDamp * kS;
    const ee_float linearDamp = -data.linDampConstant * kS;
    car.impulse.x = car.impulse.x + car.vel.x * linearDamp;
    car.impulse.y = car.impulse.y + car.vel.y * linearDamp;
    car.impulse.z = car.impulse.z + car.vel.z * linearDamp;
    car.angImpulse.x = car.angImpulse.x + car.angVel.x * spinDamp;
    car.angImpulse.y = car.angImpulse.y + car.angVel.y * spinDamp;
    car.angImpulse.z = car.angImpulse.z + car.angVel.z * spinDamp;
    car.impulse.y = car.impulse.y - G(FPhysLib).gravity * data.mass * kS;
}

} // namespace rc

// Random angles can exceed the range of the ported sine and cosine.
RC_DECOMP_FN_NOFUZZ(FMatRotateY)
RC_DECOMP_FN_NOFUZZ(FUpdateOrientationFromAngVel)
RC_DECOMP_FN(FPhysLibInit)
RC_DECOMP_FN_SETUP(FPhysLibInitTESTCarData, rc::fuzz::physicsCarCopy)
RC_DECOMP_FN_SETUP(FPhysLibInitTESTCarSpheres, rc::fuzz::physicsCarCopy)
RC_DECOMP_FN(FPhysLibTerm)
// These follow pointers held inside the car, so random inputs would only produce meaningless addresses.
RC_DECOMP_FN_NOFUZZ(FPhysLibSphereInitFixup)
RC_DECOMP_FN_NOFUZZ(FInitialiseCar)
RC_DECOMP_FN_SETUP_HEAP(FPhysLibCollisionReponse, rc::fuzz::physicsRace)
RC_DECOMP_FN_NOFUZZ(FPhysLibAddCar)
RC_DECOMP_FN_NOFUZZ(FPhysLibDeleteCar)
RC_DECOMP_FN_SETUP(FPhysLibSimulation, rc::fuzz::physicsCars)
RC_DECOMP_FN_SETUP_HEAP(FCollisionResponse, rc::fuzz::physicsResponse)
RC_DECOMP_FN_SETUP_HEAP(FCollisionResponseOther, rc::fuzz::physicsResponse)
RC_DECOMP_FN_SETUP_HEAP(FCollisionResponseCarCar, rc::fuzz::physicsResponse)
RC_DECOMP_FN_NOFUZZ(FPhysLibUpdateCarSpheres)
RC_DECOMP_FN_NOFUZZ(FUpdateCarWheels)
RC_DECOMP_FN_NOFUZZ(FUpdateCarBody)
RC_DECOMP_FN_NOFUZZ(FCalcCarWheelForces)
RC_DECOMP_FN_NOFUZZ(FCalcBoatMotorForces)
RC_DECOMP_FN_NOFUZZ(FCalcWaterForces)
RC_DECOMP_FN_NOFUZZ(FCalcCarBodyForces)

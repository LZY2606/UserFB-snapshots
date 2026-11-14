
#include "config.h"

#include <cmath>

#include "AL/al.h"
#include "AL/efx.h"

#include "alc/effects/base.h"
#include "effects.h"

#ifdef ALSOFT_EAX
#include "alnumeric.h"
#include "al/eax/exception.h"
#include "al/eax/utils.h"
#endif // ALSOFT_EAX


namespace {

void Pshifter_setParamf(EffectProps *props, ALenum param, float val)
{
    switch(param)
    {
    case AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT:
        if(!std::isfinite(val) || !(val >= AL_PITCH_SHIFTER_MIN_TRANSIENT_THRESHOLD_SOFT
            && val <= AL_PITCH_SHIFTER_MAX_TRANSIENT_THRESHOLD_SOFT))
            throw effect_exception{AL_INVALID_VALUE,
                "Pitch shifter transient threshold out of range"};
        props->Pshifter.TransientThreshold = val;
        break;

    default:
        throw effect_exception{AL_INVALID_ENUM, "Invalid pitch shifter float property 0x%04x",
            param};
    }
}
void Pshifter_setParamfv(EffectProps *props, ALenum param, const float *vals)
{ Pshifter_setParamf(props, param, vals[0]); }

void Pshifter_setParami(EffectProps *props, ALenum param, int val)
{
    switch(param)
    {
    case AL_PITCH_SHIFTER_COARSE_TUNE:
        if(!(val >= AL_PITCH_SHIFTER_MIN_COARSE_TUNE && val <= AL_PITCH_SHIFTER_MAX_COARSE_TUNE))
            throw effect_exception{AL_INVALID_VALUE, "Pitch shifter coarse tune out of range"};
        props->Pshifter.CoarseTune = val;
        break;

    case AL_PITCH_SHIFTER_FINE_TUNE:
        if(!(val >= AL_PITCH_SHIFTER_MIN_FINE_TUNE && val <= AL_PITCH_SHIFTER_MAX_FINE_TUNE))
            throw effect_exception{AL_INVALID_VALUE, "Pitch shifter fine tune out of range"};
        props->Pshifter.FineTune = val;
        break;

    case AL_PITCH_SHIFTER_PHASE_LOCK_SOFT:
        if(!(val >= AL_PITCH_SHIFTER_MIN_PHASE_LOCK_SOFT
            && val <= AL_PITCH_SHIFTER_MAX_PHASE_LOCK_SOFT))
            throw effect_exception{AL_INVALID_VALUE, "Pitch shifter phase lock out of range"};
        props->Pshifter.PhaseLock = val;
        break;

    default:
        throw effect_exception{AL_INVALID_ENUM, "Invalid pitch shifter integer property 0x%04x",
            param};
    }
}
void Pshifter_setParamiv(EffectProps *props, ALenum param, const int *vals)
{ Pshifter_setParami(props, param, vals[0]); }

void Pshifter_getParami(const EffectProps *props, ALenum param, int *val)
{
    switch(param)
    {
    case AL_PITCH_SHIFTER_COARSE_TUNE:
        *val = props->Pshifter.CoarseTune;
        break;
    case AL_PITCH_SHIFTER_FINE_TUNE:
        *val = props->Pshifter.FineTune;
        break;
    case AL_PITCH_SHIFTER_PHASE_LOCK_SOFT:
        *val = props->Pshifter.PhaseLock;
        break;

    default:
        throw effect_exception{AL_INVALID_ENUM, "Invalid pitch shifter integer property 0x%04x",
            param};
    }
}
void Pshifter_getParamiv(const EffectProps *props, ALenum param, int *vals)
{ Pshifter_getParami(props, param, vals); }

void Pshifter_getParamf(const EffectProps *props, ALenum param, float *val)
{
    switch(param)
    {
    case AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT:
        *val = props->Pshifter.TransientThreshold;
        break;

    default:
        throw effect_exception{AL_INVALID_ENUM, "Invalid pitch shifter float property 0x%04x",
            param};
    }
}
void Pshifter_getParamfv(const EffectProps *props, ALenum param, float *vals)
{ Pshifter_getParamf(props, param, vals); }

EffectProps genDefaultProps() noexcept
{
    EffectProps props{};
    props.Pshifter.CoarseTune = AL_PITCH_SHIFTER_DEFAULT_COARSE_TUNE;
    props.Pshifter.FineTune   = AL_PITCH_SHIFTER_DEFAULT_FINE_TUNE;
    props.Pshifter.PhaseLock  = AL_PITCH_SHIFTER_DEFAULT_PHASE_LOCK_SOFT;
    props.Pshifter.TransientThreshold = AL_PITCH_SHIFTER_DEFAULT_TRANSIENT_THRESHOLD_SOFT;
    return props;
}

} // namespace

DEFINE_ALEFFECT_VTABLE(Pshifter);

const EffectProps PshifterEffectProps{genDefaultProps()};

#ifdef ALSOFT_EAX
namespace {

using PitchShifterCommitter = EaxCommitter<EaxPitchShifterCommitter>;

struct CoarseTuneValidator {
    void operator()(long lCoarseTune) const
    {
        eax_validate_range<PitchShifterCommitter::Exception>(
            "Coarse Tune",
            lCoarseTune,
            EAXPITCHSHIFTER_MINCOARSETUNE,
            EAXPITCHSHIFTER_MAXCOARSETUNE);
    }
}; // CoarseTuneValidator

struct FineTuneValidator {
    void operator()(long lFineTune) const
    {
        eax_validate_range<PitchShifterCommitter::Exception>(
            "Fine Tune",
            lFineTune,
            EAXPITCHSHIFTER_MINFINETUNE,
            EAXPITCHSHIFTER_MAXFINETUNE);
    }
}; // FineTuneValidator

struct AllValidator {
    void operator()(const EAXPITCHSHIFTERPROPERTIES& all) const
    {
        CoarseTuneValidator{}(all.lCoarseTune);
        FineTuneValidator{}(all.lFineTune);
    }
}; // AllValidator

} // namespace

template<>
struct PitchShifterCommitter::Exception : public EaxException {
    explicit Exception(const char *message) : EaxException{"EAX_PITCH_SHIFTER_EFFECT", message}
    { }
};

template<>
[[noreturn]] void PitchShifterCommitter::fail(const char *message)
{
    throw Exception{message};
}

template<>
bool PitchShifterCommitter::commit(const EaxEffectProps &props)
{
    if(props.mType == mEaxProps.mType
        && mEaxProps.mPitchShifter.lCoarseTune == props.mPitchShifter.lCoarseTune
        && mEaxProps.mPitchShifter.lFineTune == props.mPitchShifter.lFineTune)
        return false;

    mEaxProps = props;

    mAlProps.Pshifter.CoarseTune = static_cast<int>(mEaxProps.mPitchShifter.lCoarseTune);
    mAlProps.Pshifter.FineTune = static_cast<int>(mEaxProps.mPitchShifter.lFineTune);

    return true;
}

template<>
void PitchShifterCommitter::SetDefaults(EaxEffectProps &props)
{
    props.mType = EaxEffectType::PitchShifter;
    props.mPitchShifter.lCoarseTune = EAXPITCHSHIFTER_DEFAULTCOARSETUNE;
    props.mPitchShifter.lFineTune = EAXPITCHSHIFTER_DEFAULTFINETUNE;
}

template<>
void PitchShifterCommitter::Get(const EaxCall &call, const EaxEffectProps &props)
{
    switch(call.get_property_id())
    {
    case EAXPITCHSHIFTER_NONE: break;
    case EAXPITCHSHIFTER_ALLPARAMETERS: call.set_value<Exception>(props.mPitchShifter); break;
    case EAXPITCHSHIFTER_COARSETUNE: call.set_value<Exception>(props.mPitchShifter.lCoarseTune); break;
    case EAXPITCHSHIFTER_FINETUNE: call.set_value<Exception>(props.mPitchShifter.lFineTune); break;
    default: fail_unknown_property_id();
    }
}

template<>
void PitchShifterCommitter::Set(const EaxCall &call, EaxEffectProps &props)
{
    switch(call.get_property_id())
    {
    case EAXPITCHSHIFTER_NONE: break;
    case EAXPITCHSHIFTER_ALLPARAMETERS: defer<AllValidator>(call, props.mPitchShifter); break;
    case EAXPITCHSHIFTER_COARSETUNE: defer<CoarseTuneValidator>(call, props.mPitchShifter.lCoarseTune); break;
    case EAXPITCHSHIFTER_FINETUNE: defer<FineTuneValidator>(call, props.mPitchShifter.lFineTune); break;
    default: fail_unknown_property_id();
    }
}

#endif // ALSOFT_EAX

#include "DSHUD.h"
#include "DSGameMode.h"
#include "DSFlightPawn.h"
#include "DSEnemyAircraft.h"
#include "DSPlayerController.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GlobalRenderResources.h"

namespace
{
    const FLinearColor Ink(.015f, .029f, .042f, .91f);
    const FLinearColor Cyan(.20f, .86f, .90f, 1.f);
    const FLinearColor White(.91f, .95f, .95f, 1.f);
    const FLinearColor Muted(.49f, .62f, .66f, 1.f);
    const FLinearColor Rule(.19f, .34f, .39f, .80f);
    const FLinearColor Amber(1.f, .64f, .27f, 1.f);
    const FLinearColor Red(1.f, .30f, .25f, 1.f);
    FString ClockText(float Seconds)
    {
        const int32 Whole = FMath::Max(0, FMath::CeilToInt(Seconds));
        return FString::Printf(TEXT("%02d:%02d"), Whole / 60, Whole % 60);
    }
}

void ADSHUD::BeginPlay()
{
    Super::BeginPlay();
    DisplayFont = LoadObject<UFont>(nullptr, TEXT("/Game/UI/Fonts/OrbitronFont.OrbitronFont"));
    BodyFont = LoadObject<UFont>(nullptr, TEXT("/Game/UI/Fonts/RajdhaniFont.RajdhaniFont"));
    if (!BodyFont) BodyFont = DisplayFont ? DisplayFont.Get() : GEngine ? GEngine->GetMediumFont() : nullptr;
    if (!DisplayFont) DisplayFont = BodyFont;
}

void ADSHUD::Text(const FString& Value, float X, float Y, float Size, FLinearColor Color, bool bDisplay, bool bCenter)
{
    UFont* Font = bDisplay ? DisplayFont.Get() : BodyFont.Get();
    if (!Canvas || !Font || Value.IsEmpty()) return;
    const float FontScale = Size / FMath::Max(1.f, static_cast<float>(Font->GetMaxCharHeight()));
    float TextWidth = 0.f, TextHeight = 0.f;
    if (bCenter) Canvas->StrLen(Font, Value, TextWidth, TextHeight);
    FCanvasTextItem Item(Offset + FVector2D((X - (bCenter ? TextWidth * FontScale * .5f : 0.f)) * UIScale, Y * UIScale), FText::FromString(Value), Font, Color);
    Item.Scale = FVector2D(FontScale * UIScale);
    Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, .72f), FVector2D(1.f, 1.f));
    Canvas->DrawItem(Item);
}

void ADSHUD::Rect(float X, float Y, float W, float H, FLinearColor Color)
{
    DrawRect(Color, Offset.X + X * UIScale, Offset.Y + Y * UIScale, W * UIScale, H * UIScale);
}

void ADSHUD::Line(float X1, float Y1, float X2, float Y2, FLinearColor Color, float Width)
{
    DrawLine(Offset.X + X1 * UIScale, Offset.Y + Y1 * UIScale, Offset.X + X2 * UIScale, Offset.Y + Y2 * UIScale, Color, Width * UIScale);
}

void ADSHUD::Triangle(FVector2D A, FVector2D B, FVector2D C, FLinearColor Color)
{
    FCanvasTriangleItem Item(Offset + A * UIScale, Offset + B * UIScale, Offset + C * UIScale, GWhiteTexture);
    Item.SetColor(Color);
    Item.BlendMode = SE_BLEND_Translucent;
    Canvas->DrawItem(Item);
}

void ADSHUD::Frame(float X, float Y, float W, float H, FLinearColor Color, float Corner)
{
    Line(X, Y, X + Corner, Y, Color, 2); Line(X, Y, X, Y + Corner, Color, 2);
    Line(X + W - Corner, Y, X + W, Y, Color, 2); Line(X + W, Y, X + W, Y + Corner, Color, 2);
    Line(X, Y + H - Corner, X, Y + H, Color, 2); Line(X, Y + H, X + Corner, Y + H, Color, 2);
    Line(X + W - Corner, Y + H, X + W, Y + H, Color, 2); Line(X + W, Y + H - Corner, X + W, Y + H, Color, 2);
}

void ADSHUD::Panel(float X, float Y, float W, float H, const FString& Label)
{
    Rect(X, Y, W, H, Ink);
    Rect(X, Y, 3, H, Cyan.CopyWithNewOpacity(.65f));
    Line(X + 18, Y + 39, X + W - 18, Y + 39, Rule);
    Text(Label, X + 20, Y + 12, 15, Muted, true);
}

void ADSHUD::Button(FName Name, const FString& Label, float X, float Y, float W, float H, bool bPrimary, bool bSelected)
{
    const bool bHover = Mouse.X >= X && Mouse.X <= X + W && Mouse.Y >= Y && Mouse.Y <= Y + H;
    const FLinearColor Accent = bPrimary || bSelected || bHover ? Cyan : Rule;
    Rect(X, Y, W, H, bPrimary ? Cyan : bHover ? FLinearColor(.055f, .17f, .20f, .98f) : Ink);
    if (!bPrimary) { Line(X, Y, X + W, Y, Accent); Line(X, Y + H, X + W, Y + H, Accent); }
    Rect(X, Y, 3, H, Accent);
    if (!Label.IsEmpty()) Text(Label, X + 22, Y + (H - 19) * .5f, 19, bPrimary ? Ink : White, true);
    if (bPrimary)
    {
        Line(X + W - 47, Y + H * .5f, X + W - 23, Y + H * .5f, Ink, 2);
        Line(X + W - 31, Y + H * .5f - 7, X + W - 23, Y + H * .5f, Ink, 2);
        Line(X + W - 31, Y + H * .5f + 7, X + W - 23, Y + H * .5f, Ink, 2);
    }
    AddHitBox(Offset + FVector2D(X, Y) * UIScale, FVector2D(W, H) * UIScale, Name, true, 10);
}

void ADSHUD::Bar(float X, float Y, float W, float H, float Fraction, FLinearColor Color)
{
    Rect(X, Y, W, H, FLinearColor(.09f, .17f, .20f, .94f));
    Rect(X, Y, W * FMath::Clamp(Fraction, 0.f, 1.f), H, Color);
    for (int32 I = 1; I < 10; ++I) Rect(X + W * I / 10.f, Y, 2, H, Ink);
}

void ADSHUD::AmmoIcon(float X, float Y, bool bMissile, FLinearColor Color)
{
    if (bMissile)
    {
        Triangle(FVector2D(X + 13, Y), FVector2D(X + 7, Y + 12), FVector2D(X + 19, Y + 12), Color);
        Rect(X + 8, Y + 12, 10, 28, Color);
        Triangle(FVector2D(X + 8, Y + 22), FVector2D(X, Y + 38), FVector2D(X + 8, Y + 35), Color);
        Triangle(FVector2D(X + 18, Y + 22), FVector2D(X + 26, Y + 38), FVector2D(X + 18, Y + 35), Color);
        Rect(X + 10, Y + 43, 6, 5, Color.CopyWithNewOpacity(.6f));
    }
    else
    {
        Triangle(FVector2D(X + 9, Y), FVector2D(X + 3, Y + 11), FVector2D(X + 15, Y + 11), Color);
        Rect(X + 3, Y + 11, 12, 27, Color); Rect(X, Y + 40, 18, 5, Color);
        Line(X + 6, Y + 13, X + 6, Y + 33, Ink);
    }
}

void ADSHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas) return;
    UIScale = FMath::Min(Canvas->ClipX / 1920.f, Canvas->ClipY / 1080.f);
    Offset = FVector2D((Canvas->ClipX - 1920.f * UIScale) * .5f, (Canvas->ClipY - 1080.f * UIScale) * .5f);
    HitBoxMap.Reset();
    HitBoxHits.Reset();
    Mouse = FVector2D(-1000, -1000);
    ADSPlayerController* PC = Cast<ADSPlayerController>(PlayerOwner);
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!PC || !Mode) return;
    float MX = 0.f, MY = 0.f;
    if (PC->GetMousePosition(MX, MY)) Mouse = (FVector2D(MX, MY) - Offset) / UIScale;
    if (Mode->Phase == EDSPhase::Menu) Menu(Mode);
    else if (Mode->Phase == EDSPhase::Flying && Mode->Player) Combat(Mode, Mode->Player);
    else if (Mode->Phase == EDSPhase::Defeat) Debrief(Mode);
    if (PC->bPaused) Paused(Mode);
    if (PC->bShowSettings || PC->bShowControls)
    {
        // Only the visible modal receives clicks; hidden menu buttons cannot fire.
        HitBoxMap.Reset();
        HitBoxHits.Reset();
        if (PC->bShowSettings) Settings(PC); else Controls();
    }
}

void ADSHUD::NotifyHitBoxClick(FName BoxName)
{
    Super::NotifyHitBoxClick(BoxName);
    if (ADSPlayerController* PC = Cast<ADSPlayerController>(PlayerOwner)) PC->HandleMenuAction(BoxName);
}

void ADSHUD::Menu(ADSGameMode* Mode)
{
    // The aircraft remains visible over the open center and right of the apron.
    Rect(0, 0, 1920, 58, Ink);
    Text(TEXT("SENTINEL FLIGHT  /  READY ALERT"), 64, 20, 17, Cyan, true);
    Text(TEXT("GOBI SECTOR 07   /   LOCAL 14:32"), 1536, 22, 14, Muted);
    Rect(48, 104, 614, 634, FLinearColor(.012f, .027f, .040f, .92f));
    Line(76, 139, 128, 139, Cyan, 3);
    Text(TEXT("OPERATION GLASS HORIZON"), 146, 130, 17, Cyan, true);
    Text(TEXT("DUNE"), 76, 180, 76, White, true);
    Text(TEXT("SENTINEL"), 76, 268, 76, White, true);
    Text(TEXT("One airfield. One open corridor."), 80, 379, 24, White);
    Text(TEXT("Keep the runway alive."), 80, 416, 22, Muted);
    Button(TEXT("start"), TEXT("START SORTIE"), 80, 480, 548, 72, true);
    Text(TEXT("ENTER TO DEPLOY   /   EXPERT FLIGHT"), 84, 570, 14, Muted, true);
    Button(TEXT("controls"), TEXT("CONTROLS"), 80, 619, 176, 54);
    Button(TEXT("settings"), TEXT("SETTINGS"), 266, 619, 176, 54);
    Button(TEXT("quit"), TEXT("QUIT"), 452, 619, 176, 54);
    Text(FString::Printf(TEXT("BEST DEFENSE   %06d"), Mode->HighScore), 84, 700, 16, Amber, true);
    Text(TEXT("AIRFRAME SELECTION"), 64, 776, 19, White, true);
    Line(370, 786, 1856, 786, Rule);
    const TCHAR* Notes[] = { TEXT("AGILITY / QUICK INTERCEPTS"), TEXT("BALANCE / FLEXIBLE RESPONSE"), TEXT("ENDURANCE / HEAVY DEFENSE") };
    for (int32 I = 0; I < 3; ++I)
    {
        if (!Mode->Aircraft.IsValidIndex(I)) continue;
        const FDSAircraftSpec& Spec = Mode->Aircraft[I];
        const float X = 64.f + I * 604.f;
        const bool bChosen = Mode->SelectedAircraft == I;
        Button(FName(*FString::Printf(TEXT("aircraft%d"), I)), TEXT(""), X, 816, 584, 186, false, bChosen);
        Text(FString::Printf(TEXT("0%d"), I + 1), X + 24, 837, 18, bChosen ? Cyan : Muted, true);
        Text(Spec.Name.ToUpper(), X + 74, 835, 25, White, true);
        Text(bChosen ? TEXT("SELECTED") : TEXT("AVAILABLE"), X + 434, 839, 12, bChosen ? Cyan : Muted, true);
        Text(Notes[I], X + 25, 875, 15, Muted, true);
        Line(X + 24, 908, X + 560, 908, Rule);
        Text(FString::Printf(TEXT("HULL  %03.0f"), Spec.MaxHealth), X + 25, 929, 18, White, true);
        AmmoIcon(X + 212, 928, false, Cyan); Text(FString::FromInt(Spec.Cannon), X + 245, 933, 21, White, true);
        AmmoIcon(X + 384, 925, true, Cyan); Text(FString::FromInt(Spec.Missiles), X + 425, 933, 21, White, true);
        if (bChosen) Rect(X + 24, 995, 536, 3, Cyan);
    }
    if (Mode->Aircraft.IsValidIndex(Mode->SelectedAircraft))
    {
        const FDSAircraftSpec& Spec = Mode->Aircraft[Mode->SelectedAircraft];
        Text(TEXT("ARMED AND READY"), 1510, 137, 17, Cyan, true);
        Text(Spec.Name.ToUpper(), 1510, 171, 26, White, true);
        Text(Spec.Role.ToUpper(), 1510, 209, 15, White);
        Line(1510, 244, 1850, 244, Cyan);
        Text(FString::Printf(TEXT("MAX AIRSPEED  %.0f KM/H"), Spec.MaxSpeed * 3.6f), 1510, 266, 16, Muted, true);
    }
    Text(TEXT("HOLD THE LAST EVACUATION CORRIDOR"), 64, 1040, 14, White, true);
    Text(TEXT("SINGLE PILOT  /  SURVIVAL OPERATIONS"), 1460, 1040, 14, Muted);
}

void ADSHUD::Combat(ADSGameMode* Mode, ADSFlightPawn* Player)
{
    const float Hull = Player->Health / FMath::Max(1.f, Player->Spec.MaxHealth);
    const float Airport = Mode->AirportHealth / FMath::Max(1.f, Mode->AirportMaxHealth);
    Panel(48, 42, 360, 151, TEXT("SENTINEL 01  /  AIRFRAME"));
    Text(Player->Spec.Name.ToUpper(), 69, 97, 21, White, true);
    Text(FString::Printf(TEXT("%03.0f"), FMath::Max(0.f, Player->Health)), 326, 94, 29, Hull < .3f ? Red : White, true);
    Bar(69, 145, 318, 11, Hull, Hull < .3f ? Red : Cyan);
    Text(TEXT("HULL INTEGRITY"), 69, 168, 11, Muted, true);
    Panel(1512, 42, 360, 151, TEXT("DEFEND  /  CORRIDOR AIRFIELD"));
    Text(FString::Printf(TEXT("%03.0f%%"), FMath::Max(0.f, Airport) * 100.f), 1533, 94, 34, Airport < .3f ? Red : White, true);
    Text(TEXT("RUNWAY STATUS"), 1667, 109, 14, Airport < .3f ? Red : Muted);
    Bar(1533, 145, 318, 11, Airport, Airport < .3f ? Red : Cyan);
    Text(TEXT("NEVER LET THE CORRIDOR CLOSE"), 1533, 168, 11, Muted, true);
    Rect(720, 28, 480, 119, Ink);
    Text(FString::Printf(TEXT("%06d"), Mode->Score), 960, 44, 42, White, true, true);
    Text(FString::Printf(TEXT("WAVE %02d   /   NEXT %s   /   HELD %s"), Mode->Wave, *ClockText(Mode->SecondsToNextWave()), *ClockText(Mode->Elapsed)), 960, 107, 15, Cyan, false, true);

    // The gun follows the airframe, while the chase camera can lag or look away.
    // Hide off-screen cues rather than pinning a false firing direction to an edge.
    auto ProjectAimPoint = [&](const FVector& World, FVector2D& Point)
    {
        FVector2D Screen;
        if (!PlayerOwner->ProjectWorldLocationToScreen(World, Screen)) return false;
        Point = (Screen - Offset) / UIScale;
        return Point.X > 40.f && Point.X < 1880.f && Point.Y > 215.f && Point.Y < 840.f;
    };
    const ADSEnemyAircraft* AimTarget = Player->Target.Get();
    const bool bLiveAimTarget = IsValid(AimTarget) && !AimTarget->bDying && AimTarget->Health > 0.f;
    const float AimRangeCm = bLiveAimTarget ? static_cast<float>(FVector::Dist(AimTarget->GetActorLocation(), Player->GetActorLocation())) : 100000.f;
    const FVector AimPoint = Player->GetActorLocation() + Player->GetActorForwardVector() * FMath::Clamp(AimRangeCm, 30000.f, 200000.f);
    const FLinearColor Sight = Cyan.CopyWithNewOpacity(.8f);
    FVector2D Boresight;
    if (ProjectAimPoint(AimPoint, Boresight))
    {
        Line(Boresight.X - 36, Boresight.Y, Boresight.X - 15, Boresight.Y, Sight, 2);
        Line(Boresight.X + 15, Boresight.Y, Boresight.X + 36, Boresight.Y, Sight, 2);
        Line(Boresight.X, Boresight.Y - 27, Boresight.X, Boresight.Y - 14, Sight, 2);
        Line(Boresight.X, Boresight.Y + 14, Boresight.X, Boresight.Y + 27, Sight, 2);
        Rect(Boresight.X - 2, Boresight.Y - 2, 4, 4, White);
    }
    if (bLiveAimTarget && AimRangeCm < 250000.f)
    {
        // Range and velocity are centimetres; cannon speed includes aircraft speed.
        const float FlightSeconds = AimRangeCm / ((1450.f + FMath::Max(0.f, Player->Speed)) * 100.f);
        const FVector LeadPoint = AimTarget->GetActorLocation() + AimTarget->GetVelocity() * FlightSeconds;
        FVector2D Lead;
        if (ProjectAimPoint(LeadPoint, Lead))
        {
            constexpr int32 Segments = 20;
            constexpr float Radius = 10.f;
            for (int32 I = 0; I < Segments; ++I)
            {
                const float A = I * 2.f * PI / Segments, B = (I + 1) * 2.f * PI / Segments;
                Line(Lead.X + FMath::Cos(A) * Radius, Lead.Y + FMath::Sin(A) * Radius,
                    Lead.X + FMath::Cos(B) * Radius, Lead.Y + FMath::Sin(B) * Radius, White.CopyWithNewOpacity(.8f), 1.5f);
            }
        }
    }
    const int32 Heading = (FMath::RoundToInt(Player->GetActorRotation().Yaw) + 360) % 360;
    Text(FString::Printf(TEXT("%03d"), Heading), 960, 170, 18, White, true, true);
    for (int32 I = -4; I <= 4; ++I) Line(960 + I * 44, 197, 960 + I * 44, I == 0 ? 211 : 204, I == 0 ? Cyan : Rule, 1);

    if (ADSEnemyAircraft* Target = Player->Target.Get())
    {
        FVector2D Screen;
        if (!Target->bDying && PlayerOwner->ProjectWorldLocationToScreen(Target->GetActorLocation(), Screen))
        {
            const FVector2D P = (Screen - Offset) / UIScale;
            if (P.X > 70 && P.X < 1850 && P.Y > 215 && P.Y < 840)
            {
                const bool bLocked = Player->LockProgress >= 1.f;
                const FLinearColor Color = bLocked ? Amber : Cyan;
                Frame(P.X - 42, P.Y - 42, 84, 84, Color, 15);
                const float Range = FVector::Dist(Target->GetActorLocation(), Player->GetActorLocation()) / 100000.f;
                Text(Target->bBomber ? TEXT("BOMBER") : TEXT("FIGHTER"), P.X, P.Y - 69, 15, Color, true, true);
                Text(FString::Printf(TEXT("%.2f KM"), Range), P.X, P.Y + 53, 16, White, true, true);
                Bar(P.X - 42, P.Y + 81, 84, 5, Player->LockProgress, Color);
                Text(bLocked ? TEXT("LOCKED") : TEXT("TRACKING"), P.X, P.Y + 95, 12, Color, true, true);
            }
        }
    }

    Radar(Mode, Player);
    Panel(736, 914, 448, 124, TEXT("WEAPONS  /  FINITE LOADOUT"));
    AmmoIcon(761, 971, false, Player->CannonAmmo > 0 ? Cyan : Red);
    Text(FString::Printf(TEXT("%04d"), Player->CannonAmmo), 803, 974, 34, Player->CannonAmmo > 0 ? White : Red, true);
    Text(TEXT("GUN"), 805, 1016, 11, Muted, true);
    Line(958, 966, 958, 1023, Rule);
    AmmoIcon(985, 966, true, Player->MissileAmmo > 0 ? Cyan : Red);
    Text(FString::Printf(TEXT("%02d"), Player->MissileAmmo), 1033, 974, 34, Player->MissileAmmo > 0 ? White : Red, true);
    Text(TEXT("MISSILE"), 1035, 1016, 11, Muted, true);

    Panel(1512, 874, 360, 164, TEXT("FLIGHT TELEMETRY"));
    const float Altitude = FMath::Max(0.f, (Player->GetActorLocation().Z - Mode->GroundHeightAt(Player->GetActorLocation())) / 100.f);
    Text(FString::Printf(TEXT("%04.0f"), Player->Speed * 3.6f), 1535, 931, 35, Player->bStalling ? Red : White, true);
    Text(TEXT("KM/H"), 1537, 982, 13, Muted, true);
    Line(1685, 929, 1685, 1007, Rule);
    Text(FString::Printf(TEXT("%04.0f"), Altitude), 1710, 931, 35, White, true);
    Text(TEXT("ALT AGL / M"), 1712, 982, 13, Muted, true);
    Text(TEXT("ESC  PAUSE / FLIGHT MANUAL"), 1512, 1051, 12, Muted, true);

    FString Warning;
    if (Player->bMissileWarning) Warning = TEXT("MISSILE INBOUND  /  BREAK");
    else if (Player->bStalling) Warning = TEXT("STALL  /  LOWER NOSE + ACCELERATE");
    else if (Player->bHighG) Warning = TEXT("HIGH-G TURN  /  AIRSPEED BLEED");
    if (!Warning.IsEmpty())
    {
        Rect(681, 232, 558, 47, Ink);
        const FLinearColor WarningColor = Player->bHighG && !Player->bMissileWarning ? Amber : Red;
        Line(681, 232, 1239, 232, WarningColor, 2);
        Text(Warning, 960, 247, 17, WarningColor, true, true);
    }
    if (Mode->bRearming)
    {
        Rect(722, 852, 476, 40, Ink);
        Text(TEXT("AIRFIELD LINK  /  REARMING"), 960, 864, 16, Cyan, true, true);
    }
    else if (Player->CannonAmmo == 0 || Player->MissileAmmo == 0)
        Text(TEXT("RETURN TO AIRFIELD TO REARM"), 960, 864, 16, Amber, true, true);
    if (!Mode->RadioLine.IsEmpty())
    {
        Rect(590, 750, 740, 80, Ink);
        Rect(590, 750, 3, 80, Cyan);
        Text(TEXT("CORRIDOR CONTROL"), 610, 762, 12, Cyan, true);
        Text(Mode->RadioLine.Left(80), 610, 790, 18, White);
    }
}

void ADSHUD::Radar(ADSGameMode* Mode, ADSFlightPawn* Player)
{
    Panel(48, 732, 300, 306, TEXT("TACTICAL RADAR  /  6 KM"));
    const FVector2D Center(198, 903);
    constexpr float Radius = 108.f;
    for (int32 Ring = 1; Ring <= 2; ++Ring)
        for (int32 I = 0; I < 48; ++I)
        {
            const float A = I * 2.f * PI / 48.f, B = (I + 1) * 2.f * PI / 48.f;
            const float R = Radius * Ring / 2.f;
            Line(Center.X + FMath::Cos(A) * R, Center.Y + FMath::Sin(A) * R, Center.X + FMath::Cos(B) * R, Center.Y + FMath::Sin(B) * R, Rule);
        }
    Line(Center.X - Radius, Center.Y, Center.X + Radius, Center.Y, Rule);
    Line(Center.X, Center.Y - Radius, Center.X, Center.Y + Radius, Rule);
    const FRotator Bearing(0, Player->GetActorRotation().Yaw, 0);
    auto RadarPoint = [&](const FVector& World)
    {
        const FVector Local = Bearing.UnrotateVector(World - Player->GetActorLocation());
        FVector2D P(Local.Y, -Local.X);
        P *= Radius / 600000.f;
        if (P.Size() > Radius - 5) P = P.GetSafeNormal() * (Radius - 5);
        return Center + P;
    };
    const FVector2D Base = RadarPoint(Mode->AirportLocation);
    Frame(Base.X - 5, Base.Y - 5, 10, 10, Cyan, 3);
    for (const auto& Enemy : Mode->Enemies)
    {
        if (!IsValid(Enemy) || Enemy->bDying || Enemy->Health <= 0.f) continue;
        const FVector2D P = RadarPoint(Enemy->GetActorLocation());
        const FLinearColor Color = Enemy->bBomber ? Amber : Red;
        if (Enemy->bBomber) Rect(P.X - 4, P.Y - 4, 8, 8, Color);
        else Triangle(P + FVector2D(0, -5), P + FVector2D(-4, 4), P + FVector2D(4, 4), Color);
    }
    Triangle(Center + FVector2D(0, -9), Center + FVector2D(-6, 6), Center + FVector2D(6, 6), White);
    Text(TEXT("HDG UP"), 198, 1018, 10, Muted, true, true);
}

void ADSHUD::ModalBackdrop(const FString& Kicker, const FString& Title)
{
    DrawRect(FLinearColor(.006f, .013f, .022f, .86f), 0, 0, Canvas->ClipX, Canvas->ClipY);
    Rect(490, 144, 940, 790, Ink);
    Frame(490, 144, 940, 790, Cyan, 24);
    Text(Kicker, 544, 186, 16, Cyan, true);
    Text(Title, 544, 226, 40, White, true);
    Line(544, 296, 1376, 296, Rule);
}

void ADSHUD::Paused(ADSGameMode* Mode)
{
    HitBoxMap.Reset();
    HitBoxHits.Reset();
    ModalBackdrop(TEXT("SENTINEL FLIGHT  /  OPERATIONS HOLD"), TEXT("SORTIE PAUSED"));
    Text(FString::Printf(TEXT("WAVE %02d    /    HELD %s    /    SCORE %06d"), Mode->Wave, *ClockText(Mode->Elapsed), Mode->Score), 544, 328, 18, Muted, true);
    Button(TEXT("resume"), TEXT("RESUME FLIGHT"), 544, 398, 832, 70, true);
    Button(TEXT("restart"), TEXT("RESTART SORTIE"), 544, 492, 832, 61);
    Button(TEXT("controls"), TEXT("FLIGHT MANUAL"), 544, 577, 406, 61);
    Button(TEXT("settings"), TEXT("SETTINGS"), 970, 577, 406, 61);
    Button(TEXT("menu"), TEXT("RETURN TO AIRFIELD"), 544, 662, 832, 61);
    Text(TEXT("THE SIMULATION IS PAUSED."), 544, 786, 15, Muted, true);
    Text(TEXT("ESC OR ENTER TO RESUME"), 544, 858, 15, Cyan, true);
}

void ADSHUD::Debrief(ADSGameMode* Mode)
{
    ModalBackdrop(TEXT("OPERATION GLASS HORIZON  /  DEBRIEF"), TEXT("CORRIDOR LOST"));
    Text(Mode->DefeatReason.Left(65), 544, 326, 21, Amber);
    Text(TEXT("DEFENSE SCORE"), 544, 392, 15, Muted, true);
    Text(FString::Printf(TEXT("%06d"), Mode->Score), 544, 432, 64, White, true);
    Text(TEXT("TIME HELD"), 1074, 392, 15, Muted, true);
    Text(ClockText(Mode->Elapsed), 1074, 444, 40, White, true);
    Text(FString::Printf(TEXT("WAVE REACHED  %02d    /    PERSONAL BEST  %06d"), Mode->Wave, Mode->HighScore), 544, 536, 17, Cyan, true);
    Line(544, 594, 1376, 594, Rule);
    Button(TEXT("restart"), TEXT("FLY AGAIN"), 544, 638, 832, 70, true);
    Button(TEXT("menu"), TEXT("SELECT AIRFRAME"), 544, 734, 406, 61);
    Button(TEXT("quit"), TEXT("QUIT"), 970, 734, 406, 61);
    Text(TEXT("EVERY SECOND KEPT THE CORRIDOR OPEN."), 544, 858, 15, Muted, true);
}

void ADSHUD::Settings(ADSPlayerController* PC)
{
    ModalBackdrop(TEXT("PILOT PROFILE"), TEXT("SETTINGS"));
    const float X = 544, Right = 1376;
    Text(TEXT("MASTER VOLUME"), X, 348, 19, White, true);
    Button(TEXT("volume_down"), TEXT("-"), 1034, 330, 58, 54);
    Text(FString::Printf(TEXT("%03.0f%%"), PC->MasterVolume * 100.f), 1191, 345, 23, Cyan, true, true);
    Button(TEXT("volume_up"), TEXT("+"), 1318, 330, 58, 54);
    Line(X, 410, Right, 410, Rule);
    Text(TEXT("MOUSE SENSITIVITY"), X, 455, 19, White, true);
    Button(TEXT("sensitivity_down"), TEXT("-"), 1034, 437, 58, 54);
    Text(FString::Printf(TEXT("%.1f"), PC->MouseSensitivity), 1191, 452, 23, Cyan, true, true);
    Button(TEXT("sensitivity_up"), TEXT("+"), 1318, 437, 58, 54);
    Line(X, 517, Right, 517, Rule);
    Text(TEXT("INVERT MOUSE PITCH"), X, 560, 19, White, true);
    Button(TEXT("invert"), PC->bInvertMouse ? TEXT("ON") : TEXT("OFF"), 1134, 542, 242, 54, false, PC->bInvertMouse);
    Line(X, 622, Right, 622, Rule);
    Text(TEXT("WINDOW RESOLUTION"), X, 664, 19, White, true);
    Button(TEXT("resolution"), PC->ResolutionLabel(), 1034, 646, 342, 54);
    Text(TEXT("CLICK TO CYCLE  /  WINDOWED DISPLAY"), 1034, 714, 12, Muted);
    Text(PC->SettingsFeedback, X, 772, 14, Cyan, true);
    Button(TEXT("back"), TEXT("BACK"), 544, 826, 266, 60);
    Button(TEXT("apply_display"), TEXT("APPLY DISPLAY"), 970, 826, 406, 60, true);
}

void ADSHUD::Controls()
{
    ModalBackdrop(TEXT("PILOT QUALIFICATION  /  KEYBOARD + MOUSE"), TEXT("EXPERT FLIGHT"));
    struct FEntry { const TCHAR* Action; const TCHAR* Keys; };
    const FEntry Entries[] = {
        { TEXT("PITCH / ROLL"), TEXT("MOUSE + UP/DOWN / A,D") },
        { TEXT("YAW / LEVEL FLIGHT"), TEXT("Q,E / Q+E") },
        { TEXT("ACCELERATE / BRAKE"), TEXT("W / S") },
        { TEXT("HIGH-G TURN"), TEXT("W + S + PITCH") },
        { TEXT("CANNON / MISSILE"), TEXT("LMB, CTRL / RMB, SPACE") },
        { TEXT("NEXT TARGET / TARGET VIEW"), TEXT("TAB / HOLD TAB OR F") },
        { TEXT("FREE LOOK"), TEXT("ALT + MOUSE") },
        { TEXT("CAMERA / PAUSE"), TEXT("V / ESC") }
    };
    for (int32 I = 0; I < UE_ARRAY_COUNT(Entries); ++I)
    {
        const float Y = 328 + I * 49.f;
        Text(Entries[I].Action, 544, Y, 16, Muted, true);
        Text(Entries[I].Keys, 1010, Y, 17, White, true);
        Line(544, Y + 34, 1376, Y + 34, Rule.CopyWithNewOpacity(.35f));
    }
    Text(TEXT("BANK, THEN PULL. YAW TRIMS YOUR AIM."), 544, 748, 16, Cyan, true);
    Text(TEXT("Track a target until LOCKED, then launch. Fly near the airfield to rearm."), 544, 782, 17, White);
    Button(TEXT("back"), TEXT("BACK"), 544, 841, 832, 53, true);
}

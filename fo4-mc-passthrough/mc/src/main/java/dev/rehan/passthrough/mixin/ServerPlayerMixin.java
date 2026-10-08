package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Combat;
import dev.rehan.passthrough.Passthrough;
import java.util.Locale;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.level.portal.TeleportTransition;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft moving the player (ender pearls, /tp) moves the host's player too; otherwise the host would pull it back.
 * Damage to the player goes to the host, whose health is the one both games show.
 */
@Mixin(ServerPlayer.class)
abstract class ServerPlayerMixin {
	@Inject(method = "teleport(Lnet/minecraft/world/level/portal/TeleportTransition;)Lnet/minecraft/server/level/ServerPlayer;", at = @At("HEAD"))
	private void passthrough$teleported(final TeleportTransition transition, final CallbackInfoReturnable<ServerPlayer> cir) {
		if (Passthrough.active) {
			Vec3 p = transition.position();
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pteleport\",\"pos\":[%.3f,%.3f,%.3f]}", p.x, p.y, p.z));
		}
	}

	@Inject(method = "hurtServer", at = @At("HEAD"), cancellable = true)
	private void passthrough$sharedHealth(final ServerLevel level, final DamageSource source, final float amount, final CallbackInfoReturnable<Boolean> cir) {
		if (Combat.redirectDamage((ServerPlayer) (Object) this, source, amount)) {
			cir.setReturnValue(false);
		}
	}
}

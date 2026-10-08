package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Combat;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The player's healing goes to the host, whose health both games show. */
@Mixin(LivingEntity.class)
abstract class PlayerHealMixin {
	@Inject(method = "heal", at = @At("HEAD"), cancellable = true)
	private void passthrough$sharedHeal(final float amount, final CallbackInfo ci) {
		if ((Object) this instanceof ServerPlayer player && Combat.redirectHeal(player, amount)) {
			ci.cancel();
		}
	}
}

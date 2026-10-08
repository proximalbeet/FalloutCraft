package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.WorldBridge;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * A firework bursting is an explosion for the host too: its own damage to the host's people counts as an explosion's
 * (left to the host, like TNT's), so the host sets off one of its own where the rocket burst.
 */
@Mixin(FireworkRocketEntity.class)
abstract class FireworkMixin {
	@Inject(method = "explode", at = @At("HEAD"))
	private void passthrough$burst(final ServerLevel level, final CallbackInfo ci) {
		FireworkRocketEntity self = (FireworkRocketEntity) (Object) this;
		WorldBridge.onExplosion(self.position(), 2.0F, "firework_rocket");
	}
}

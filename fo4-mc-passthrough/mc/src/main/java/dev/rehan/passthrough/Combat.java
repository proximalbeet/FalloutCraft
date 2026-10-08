package dev.rehan.passthrough;

import java.util.Comparator;
import java.util.Locale;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.DamageTypeTags;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * One health between the games, and the host's guns in Minecraft.
 *
 * <ul>
 * <li>The host's health is the truth: {"t":"health","f":fraction} sets Steve's hearts. Healing on the host (a stimpak,
 * food) also feeds Steve: {"t":"feed","f":fraction healed} fills his hunger by as much.</li>
 * <li>Healing here (natural regeneration from a full hunger bar, potions, golden apples) goes to the host as
 * {"t":"pheal","d":fraction of max health}: hunger matters, and running out of it starves the host's player.</li>
 * <li>Damage Steve takes is sent to the host as {"t":"pdmg","d":fraction of max health,"src":type} instead of being
 * applied here. Explosions and falls are left out: the host's own explosion already hurts its player, and the host
 * moves Steve, so a "fall" is never his.</li>
 * <li>{"t":"shot","o":[x,y,z],"d":[dx,dy,dz],"dmg":hp}: the host's player fired along this ray. The first mob on it
 * is hurt; if a block comes first, the block breaks.</li>
 * </ul>
 */
public final class Combat {
	private static final double RANGE = 160.0;
	/** What each hotbar slot (0-8) and the off hand (40) last held, so a used-up stack comes back. */
	private static final ItemStack[] REMEMBERED = new ItemStack[41];
	private static volatile float healthFraction = 1.0F;
	private static int refillIn;

	private Combat() {
	}

	private static ServerPlayer player(final MinecraftServer s) {
		return s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
	}

	/** Host health changed. */
	public static void health(final float fraction) {
		healthFraction = Math.clamp(fraction, 0.0F, 1.0F);
	}

	/** ServerPlayer.hurtServer: true = handled here (the damage goes to the host instead). */
	public static boolean redirectDamage(final ServerPlayer player, final DamageSource source, final float amount) {
		if (!Passthrough.active) {
			return false;
		}

		if (source.is(DamageTypeTags.IS_EXPLOSION) || source.is(DamageTypeTags.IS_FALL) || amount <= 0.0F) {
			return true;
		}

		String type = source.typeHolder().unwrapKey().map(k -> k.identifier().getPath()).orElse("generic");
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pdmg\",\"d\":%.4f,\"src\":\"%s\"}", amount / player.getMaxHealth(), type));
		return true;
	}

	/** Every server tick: hearts follow the host, hunger never runs out, the hotbar refills. */
	static void tick(final MinecraftServer s) {
		ServerPlayer p = player(s);
		if (p == null || !Passthrough.active) {
			return;
		}

		// never 0 here: the host decides about death (a Minecraft death screen would sit over its picture)
		float want = Math.max(1.0F, healthFraction * p.getMaxHealth());
		if (Math.abs(p.getHealth() - want) > 0.01F) {
			p.setHealth(want);
		}

		if (--refillIn <= 0) {
			refillIn = 20;
			refill(p);
		}
	}

	/** Nothing runs out: every hotbar slot and the off hand go back to a full stack of what they last held. */
	private static void refill(final ServerPlayer p) {
		for (int slot : new int[] {0, 1, 2, 3, 4, 5, 6, 7, 8, 40}) {
			ItemStack stack = p.getInventory().getItem(slot);
			if (!stack.isEmpty()) {
				REMEMBERED[slot] = stack.copyWithCount(1);
			}

			ItemStack want = REMEMBERED[slot];
			if (want == null || stack.isEmpty() && p.containerMenu != p.inventoryMenu) {
				continue; // something is being moved around in an open screen
			}

			if (stack.isEmpty()) {
				p.getInventory().setItem(slot, want.copyWithCount(want.getMaxStackSize()));
			} else if (stack.isStackable() && stack.getCount() < stack.getMaxStackSize()) {
				stack.setCount(stack.getMaxStackSize());
			}
		}
	}

	/** The host healed its player (stimpak, food, sleep): Steve eats too, by as much. */
	public static void feed(final float fraction) {
		MinecraftServer s = WorldBridge.server();
		if (s == null || fraction <= 0.0F) {
			return;
		}

		s.execute(() -> {
			ServerPlayer p = player(s);
			if (p != null) {
				int food = Math.min(20, p.getFoodData().getFoodLevel() + Math.round(fraction * 20.0F));
				p.getFoodData().setFoodLevel(food);
				p.getFoodData().setSaturation(Math.min(food, p.getFoodData().getSaturationLevel() + fraction * 6.0F));
			}
		});
	}

	/** LivingEntity.heal on the player: true = handled (the host heals instead, and the hearts follow its health). */
	public static boolean redirectHeal(final ServerPlayer player, final float amount) {
		if (!Passthrough.active || amount <= 0.0F) {
			return false;
		}

		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pheal\",\"d\":%.4f}", amount / player.getMaxHealth()));
		return true;
	}

	/** The host's player fired: hurt the first mob on the ray, or break the block it hits first. */
	public static void shot(final Vec3 from, final Vec3 dir, final float damage) {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer shooter = player(s);
			Vec3 d = dir.normalize();
			Vec3 to = from.add(d.scale(RANGE));
			BlockHitResult block = level.clip(new ClipContext(from, to, ClipContext.Block.COLLIDER, ClipContext.Fluid.NONE, shooter));
			Vec3 end = block.getType() == HitResult.Type.MISS ? to : block.getLocation();
			AABB box = new AABB(from, end).inflate(1.0);
			Optional<LivingEntity> mob = level.getEntitiesOfClass(LivingEntity.class, box, e -> e != shooter && e.isAlive()).stream()
				.filter(e -> e.getBoundingBox().inflate(0.15).clip(from, end).isPresent())
				.min(Comparator.comparingDouble(e -> e.distanceToSqr(from)));
			if (mob.isPresent()) {
				LivingEntity e = mob.get();
				DamageSource source = shooter != null ? level.damageSources().playerAttack(shooter) : level.damageSources().generic();
				e.setInvulnerableTime(0); // automatic fire: every round counts
				e.hurtServer(level, source, damage);
				Passthrough.LOG.info("shot hit {} for {}", e.getType().toShortString(), damage);
				return;
			}

			if (block.getType() == HitResult.Type.BLOCK) {
				BlockPos pos = block.getBlockPos();
				BlockState state = level.getBlockState(pos);
				// the host's ground (barriers) and unbreakable blocks stay
				if (!state.is(Blocks.BARRIER) && state.getDestroySpeed(level, pos) >= 0.0F) {
					level.destroyBlock(pos, false, shooter, 512);
				}
			}
		});
	}
}

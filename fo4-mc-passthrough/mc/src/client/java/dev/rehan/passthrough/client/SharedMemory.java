package dev.rehan.passthrough.client;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/**
 * A tmpfs file mapped into memory. Fallout 4 runs under Proton (Wine), whose named file mappings Linux processes can't
 * see, so the frame buffer is a plain file in /dev/shm: Minecraft maps it here, and the host maps the same file through
 * Wine's Z: drive (CreateFile + CreateFileMapping), which is backed by the same pages.
 */
final class SharedMemory {
	final MemorySegment segment;

	private SharedMemory(final MemorySegment segment) {
		this.segment = segment;
	}

	static SharedMemory create(final Path path, final long size) throws IOException {
		Files.createDirectories(path.getParent());
		try (FileChannel channel = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
			// the header is rewritten below; keep the old file's pages, a reader may still have them mapped
			if (channel.size() != size) {
				channel.truncate(0L);
				channel.write(java.nio.ByteBuffer.wrap(new byte[] {0}), size - 1L);
			}

			return new SharedMemory(channel.map(FileChannel.MapMode.READ_WRITE, 0L, size, Arena.global()));
		}
	}
}

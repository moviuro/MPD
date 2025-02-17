/*
 * Copyright 2003-2022 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "PipeOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "lib/fmt/SystemError.hxx"

#include <string>
#include <stdexcept>

#include <stdio.h>

class PipeOutput final : AudioOutput {
	const std::string cmd;
	FILE *fh;

	explicit PipeOutput(const ConfigBlock &block);

public:
	static AudioOutput *Create(EventLoop &,
				   const ConfigBlock &block) {
		return new PipeOutput(block);
	}

private:
	void Open(AudioFormat &audio_format) override;

	void Close() noexcept override {
		pclose(fh);
	}

	std::size_t Play(std::span<const std::byte> src) override;
};

PipeOutput::PipeOutput(const ConfigBlock &block)
	:AudioOutput(0),
	 cmd(block.GetBlockValue("command", ""))
{
	if (cmd.empty())
		throw std::runtime_error("No \"command\" parameter specified");
}

inline void
PipeOutput::Open([[maybe_unused]] AudioFormat &audio_format)
{
	fh = popen(cmd.c_str(), "w");
	if (fh == nullptr)
		throw FmtErrno("Error opening pipe \"{}\"", cmd);
}

std::size_t
PipeOutput::Play(std::span<const std::byte> src)
{
	size_t nbytes = fwrite(src.data(), 1, src.size(), fh);
	if (nbytes == 0)
		throw MakeErrno("Write error on pipe");

	return nbytes;
}

const struct AudioOutputPlugin pipe_output_plugin = {
	"pipe",
	nullptr,
	&PipeOutput::Create,
	nullptr,
};

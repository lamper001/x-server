# X-Server Optimized Makefile
# Simplified dependencies, automatic dependency generation, improved build efficiency

# Compiler configuration
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_GNU_SOURCE -O2 -g -Iinclude -D_FORTIFY_SOURCE=2 -fstack-protector-strong
LDFLAGS = -lpthread -lm

# Security compilation options
SECURITY_CFLAGS = -fPIE -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

# Performance optimization options
PERFORMANCE_CFLAGS = -march=native -mtune=native -flto -ffast-math

# Debug options
DEBUG_CFLAGS = -DDEBUG -O0 -g3 -fsanitize=address -fsanitize=undefined

# Release options
RELEASE_CFLAGS = -DNDEBUG -O3 -fomit-frame-pointer
RELEASE_LDFLAGS = -s

# Directory definitions
SRCDIR = src
OBJDIR = obj
BINDIR = bin

# Auto-discover source files (exclude main files)
COMMON_SRCS = $(filter-out $(SRCDIR)/main%.c, $(wildcard $(SRCDIR)/*.c))
MAIN_SRCS = $(SRCDIR)/main.c

# Object files
COMMON_OBJS = $(COMMON_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
MAIN_OBJS = $(MAIN_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Target program
TARGET = $(BINDIR)/x-server

# Default target
.PHONY: all clean install uninstall test help debug release run daemon stop reload status benchmark

all: $(TARGET)

# Link rule
$(TARGET): $(COMMON_OBJS) $(MAIN_OBJS) | $(BINDIR)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo "✅ Compile completed: $@"

# Compilation rule - auto-generate dependency files
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Create directories
$(OBJDIR) $(BINDIR):
	mkdir -p $@

# Include auto-generated dependency files (avoid manual dependency maintenance)
-include $(COMMON_OBJS:.o=.d) $(MAIN_OBJS:.o=.d)

# Clean
clean:
	rm -rf $(OBJDIR) $(BINDIR) logs/* tools/bin
	@echo "🧹 Cleanup completed"

# Debug version
debug: CFLAGS += $(DEBUG_CFLAGS)
debug: clean all
	@echo "🐛 Debug version compilation completed"

# Release version
release: CFLAGS += $(RELEASE_CFLAGS) $(PERFORMANCE_CFLAGS)
release: LDFLAGS += $(RELEASE_LDFLAGS)
release: clean all
	@echo "🚀 Release version compilation completed"

# Security version
security: CFLAGS += $(SECURITY_CFLAGS)
security: clean all
	@echo "🛡️ Security version compilation completed"

# Performance version
performance: CFLAGS += $(PERFORMANCE_CFLAGS)
performance: clean all
	@echo "⚡ Performance version compilation completed"

# Test configuration
test: all
	@echo "🧪 Testing configuration file..."
	@$(TARGET) -t -c config/gateway_multiprocess.conf && echo "✅ Configuration test passed" || echo "❌ Configuration test failed"


# Run server (foreground)
run: all
	$(TARGET) -f -c config/gateway_multiprocess.conf

# Run server (daemon)
daemon: all
	$(TARGET) -c config/gateway_multiprocess.conf

# Stop server
stop:
	@echo "🛑 Stopping server..."
	@if pgrep -f "x-server.*master" > /dev/null; then \
		$(TARGET) -s stop && echo "✅ Stop command sent"; \
	else \
		echo "ℹ️  No running server found"; \
	fi

# Reload configuration
reload:
	@echo "🔄 Reloading configuration..."
	@if pgrep -f "x-server.*master" > /dev/null; then \
		$(TARGET) -s reload && echo "✅ Reload command sent"; \
	else \
		echo "ℹ️  No running server found"; \
	fi

# Show status
status:
	@echo "📊 X-Server Status:"
	@if pgrep -f "x-server.*master" > /dev/null; then \
		echo "✅ Server is running"; \
		echo "Master processes:"; \
		ps aux | grep "x-server.*master" | grep -v grep || true; \
		echo "Worker processes:"; \
		ps aux | grep "x-server.*worker" | grep -v grep || true; \
	else \
		echo "❌ Server is not running"; \
	fi

# Install to system
install: all
	@echo "📦 Installing X-Server..."
	sudo install -m 755 $(TARGET) /usr/local/bin/x-server
	sudo mkdir -p /etc/x-server /var/log/x-server
	@if [ ! -f /etc/x-server/gateway.conf ]; then \
		sudo cp config/gateway_multiprocess.conf /etc/x-server/gateway.conf; \
		echo "📄 Configuration file installed to /etc/x-server/"; \
	fi
	@echo "✅ Installation completed"
	@echo "📍 Configuration file: /etc/x-server/gateway.conf"
	@echo "📍 Log directory: /var/log/x-server/"

# Uninstall
uninstall:
	@echo "🗑️  Uninstalling X-Server..."
	sudo rm -f /usr/local/bin/x-server
	@echo "✅ Uninstallation completed"
	@echo "ℹ️  Configuration files and logs retained in system"

# Help information
help:
	@echo "🚀 X-Server Build System"
	@echo ""
	@echo "📋 Main targets:"
	@echo "  all      - Compile multi-process version (default)"
	@echo "  debug    - Compile debug version"
	@echo "  release  - Compile optimized release version"
	@echo "  security - Compile security hardened version"
	@echo "  performance - Compile performance optimized version"
	@echo "  clean    - Clean build files"
	@echo ""
	@echo "🔧 Development tools:"
	@echo "  test     - Test configuration file"
	@echo "  benchmark- Run performance comparison tests"
	@echo "  run      - Run server (foreground mode)"
	@echo "  daemon   - Run server (daemon mode)"
	@echo ""
	@echo "🎛️  Service management:"
	@echo "  stop     - Stop server"
	@echo "  reload   - Reload configuration"
	@echo "  status   - Show server status"
	@echo ""
	@echo "📦 System integration:"
	@echo "  install  - Install to system"
	@echo "  uninstall- Uninstall from system"
	@echo ""
	@echo "💡 Usage examples:"
	@echo "  make          # Compile multi-process version"
	@echo "  make run      # Run development server"
	@echo "  make test     # Test configuration"
	@echo "  make debug    # Compile debug version"
	@echo "  make install  # Install to system"

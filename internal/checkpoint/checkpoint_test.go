package checkpoint

import (
	"os"
	"testing"
	"time"
)

func TestSaveAndLoad(t *testing.T) {
	tmpFile := "test_checkpoint.json"
	defer os.Remove(tmpFile)

	// Create a test checkpoint
	original := &Checkpoint{
		Version:   "1.0",
		CreatedAt: time.Now(),
		Config: Config{
			Seeds:      []string{"https://example.com"},
			MaxDepth:   3,
			SameDomain: true,
			UserAgent:  "test-agent",
		},
		Stats: Stats{
			Fetched:  100,
			Errors:   5,
			Enqueued: 500,
			Skipped:  10,
		},
		Frontier: Frontier{
			VisitedBloomBase64: "dGVzdGRhdGE=", // base64("testdata")
			QueueLength:        2,
			Queue: []QueueItem{
				{URL: "https://example.com/page1", Depth: 1},
				{URL: "https://example.com/page2", Depth: 2},
			},
		},
	}

	// Save
	if err := Save(tmpFile, original); err != nil {
		t.Fatalf("Save failed: %v", err)
	}

	// Load
	loaded, err := Load(tmpFile)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}

	// Verify
	if loaded.Version != "1.0" {
		t.Errorf("Version mismatch: got %s, want 1.0", loaded.Version)
	}
	if loaded.Config.MaxDepth != 3 {
		t.Errorf("MaxDepth mismatch: got %d, want 3", loaded.Config.MaxDepth)
	}
	if loaded.Stats.Fetched != 100 {
		t.Errorf("Fetched mismatch: got %d, want 100", loaded.Stats.Fetched)
	}
	if loaded.Frontier.QueueLength != 2 {
		t.Errorf("QueueLength mismatch: got %d, want 2", loaded.Frontier.QueueLength)
	}
	if len(loaded.Frontier.Queue) != 2 {
		t.Errorf("Queue length mismatch: got %d, want 2", len(loaded.Frontier.Queue))
	}
}

func TestValidateConfig(t *testing.T) {
	tests := []struct {
		name    string
		current Config
		saved   Config
		wantErr bool
	}{
		{
			name: "compatible",
			current: Config{
				Seeds:      []string{"https://example.com"},
				MaxDepth:   3,
				SameDomain: true,
			},
			saved: Config{
				Seeds:      []string{"https://example.com"},
				MaxDepth:   3,
				SameDomain: true,
			},
			wantErr: false,
		},
		{
			name: "different seeds (allowed)",
			current: Config{
				Seeds:      []string{"https://example.com", "https://other.com"},
				MaxDepth:   3,
				SameDomain: true,
			},
			saved: Config{
				Seeds:      []string{"https://example.com"},
				MaxDepth:   3,
				SameDomain: true,
			},
			wantErr: false,
		},
		{
			name: "different max depth (not allowed)",
			current: Config{
				MaxDepth:   5,
				SameDomain: true,
			},
			saved: Config{
				MaxDepth:   3,
				SameDomain: true,
			},
			wantErr: true,
		},
		{
			name: "different same domain (not allowed)",
			current: Config{
				MaxDepth:   3,
				SameDomain: false,
			},
			saved: Config{
				MaxDepth:   3,
				SameDomain: true,
			},
			wantErr: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateConfig(tt.current, tt.saved)
			if (err != nil) != tt.wantErr {
				t.Errorf("ValidateConfig() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestLoadInvalidVersion(t *testing.T) {
	tmpFile := "test_invalid_version.json"
	defer os.Remove(tmpFile)

	// Create checkpoint with invalid version
	ckpt := &Checkpoint{
		Version: "99.0",
	}

	if err := Save(tmpFile, ckpt); err != nil {
		t.Fatalf("Save failed: %v", err)
	}

	// Manually set invalid version
	content := `{"version":"99.0","created_at":"2026-02-21T00:00:00Z","config":{},"stats":{},"frontier":{"queue":[]}}`
	os.WriteFile(tmpFile, []byte(content), 0644)

	// Try to load
	_, err := Load(tmpFile)
	if err == nil {
		t.Error("Expected error for invalid version, got nil")
	}
}

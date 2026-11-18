PDFLATEX ?= pdflatex

all: report.pdf docx

report.pdf: report.tex
	@if which $(PDFLATEX) >/dev/null 2>&1; then \
		echo "Building with pdflatex..."; \
		$(PDFLATEX) -interaction=nonstopmode report.tex && $(PDFLATEX) -interaction=nonstopmode report.tex; \
	else \
		echo "pdflatex not found on host. Building publication PDF with generate_report_pdf.py..."; \
		python3 generate_report_pdf.py; \
	fi

docx: report.docx

report.docx: build_docx_report.py
	python3 build_docx_report.py

clean:
	rm -f *.aux *.log *.out report.pdf report.docx

.PHONY: all clean docx
